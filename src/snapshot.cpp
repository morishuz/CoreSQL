#include "coresql/core.hpp"
#include "coresql/encoding.hpp"
#include "storage.hpp"
#include "index.hpp"

#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <unistd.h>

namespace coresql {
namespace {
constexpr std::size_t max_snapshot = detail::max_encoded_bytes;
constexpr std::string_view magic = "CORESQL5";
using encoding::Reader;
ByteView view(std::string_view s) {
    return std::as_bytes(std::span(s.data(), s.size()));
}
std::uint64_t checksum(ByteView bytes) {
    // Accidental-corruption detection only; not an authentication mechanism.
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto byte : bytes) {
        hash ^= std::to_integer<unsigned>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}
void field(Bytes& out, ByteView bytes) {
    if (bytes.size() > max_snapshot - 8 || out.size() > max_snapshot - 8 - bytes.size())
        throw Error(ErrorCode::format, "Snapshot exceeds configured encoded-size limit");
    encoding::u64(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}
ByteView field(Reader& reader) {
    auto size = reader.u64();
    if (size > reader.remaining())
        throw Error(ErrorCode::format, "Invalid snapshot field length");
    return reader.take(static_cast<std::size_t>(size));
}
std::string string(Reader& reader) {
    auto bytes = field(reader);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
std::size_t count(Reader& reader) {
    auto n = reader.u64();
    // Each table, column, and encoded value consumes at least eight bytes.
    if (n > reader.remaining() / 8)
        throw Error(ErrorCode::format, "Invalid snapshot item count");
    return static_cast<std::size_t>(n);
}
void write_value(Bytes&, const Value&);
Value read_value(Reader&, const Type&, const Registry&, bool tagged);
void write_schema(Bytes& data, const detail::Table& table) {
    const auto& columns = table.columns;
    encoding::u64(data, columns.size());
    for (const auto& column : columns) {
        field(data, view(column.name));
        field(data, view(column.type.id));
        encoding::u64(data, column.type.version);
        field(data, column.type.parameters);
        encoding::u64(data, column.primary_key);
        encoding::u64(data, column.nullable);
        field(data, view(column.index));
        encoding::u64(data, column.default_value.has_value());
        if (column.default_value)
            write_value(data, *column.default_value);
    }
    encoding::u64(data, table.index_definitions.size());
    for (const auto& d : table.index_definitions) {
        field(data, view(d.name));
        encoding::u64(data, d.unique);
        encoding::u64(data, d.columns.size());
        for (std::size_t i = 0; i < d.columns.size(); ++i) {
            field(data, view(d.columns[i]));
            encoding::u64(data, !d.descending.empty() && d.descending[i]);
        }
    }
}
detail::Table read_schema(Reader& reader, const Registry& registry, bool primary, bool indexes, bool extended,
                          bool tagged) {
    detail::Table table;
    const auto n = count(reader);
    if (!n)
        throw Error(ErrorCode::format, "Table has no columns");
    std::set<std::string> names;
    for (std::size_t i = 0; i < n; ++i) {
        auto name = string(reader), id = string(reader);
        auto version = reader.u64();
        auto parameters = field(reader);
        if (name.empty() || !names.insert(name).second || !version || version > UINT32_MAX)
            throw Error(ErrorCode::format, "Invalid column descriptor");
        Type type{std::move(id), static_cast<std::uint32_t>(version),
                  Bytes(parameters.begin(), parameters.end())};
        registry.validate(type);
        auto pk = primary ? reader.u64() : 0;
        if (pk > 1)
            throw Error(ErrorCode::format, "Invalid primary-key flag");
        auto nullable = tagged ? reader.u64() : 0;
        if (nullable > 1)
            throw Error(ErrorCode::format, "Invalid nullable flag");
        Column c{std::move(name), std::move(type), pk != 0, indexes ? string(reader) : std::string{}};
        c.nullable = nullable != 0;
        if (extended) {
            auto present = reader.u64();
            if (present > 1)
                throw Error(ErrorCode::format, "Invalid default flag");
            if (present) {
                c.default_value = read_value(reader, c.type, registry, tagged);
                detail::validate_stored(*c.default_value, c, registry);
            }
        }
        table.columns.push_back(std::move(c));
    }
    if (extended) {
        auto nindexes = count(reader);
        for (std::size_t i = 0; i < nindexes; ++i) {
            IndexDefinition d;
            d.name = string(reader);
            auto unique = reader.u64();
            if (unique > 1)
                throw Error(ErrorCode::format, "Invalid unique flag");
            d.unique = unique != 0;
            auto width = count(reader);
            for (std::size_t j = 0; j < width; ++j) {
                d.columns.push_back(string(reader));
                auto descending = reader.u64();
                if (descending > 1)
                    throw Error(ErrorCode::format, "Invalid direction flag");
                d.descending.push_back(descending != 0);
            }
            table.index_definitions.push_back(std::move(d));
        }
    }
    detail::make_indexes(table, registry);
    return table;
}
void write_value(Bytes& data, const Value& value) {
    encoding::u64(data, is_null(value));
    if (is_null(value))
        return;
    if (const auto* i = std::get_if<std::int64_t>(&value))
        encoding::u64(data, std::bit_cast<std::uint64_t>(*i));
    else if (const auto* d = std::get_if<double>(&value))
        encoding::u64(data, std::bit_cast<std::uint64_t>(*d));
    else if (const auto* s = std::get_if<std::string>(&value))
        field(data, view(*s));
    else
        field(data, std::get<Opaque>(value).bytes());
}
Value read_value(Reader& reader, const Type& type, const Registry& registry, bool tagged) {
    if (tagged) {
        auto null = reader.u64();
        if (null > 1)
            throw Error(ErrorCode::format, "Invalid NULL tag");
        if (null)
            return Null(type);
    }
    const auto layout = registry.addon(type).layout;
    if (layout == Layout::i64)
        return std::bit_cast<std::int64_t>(reader.u64());
    if (layout == Layout::f64)
        return std::bit_cast<double>(reader.u64());
    if (layout == Layout::text)
        return string(reader);
    auto payload = field(reader);
    return Opaque(type, Bytes(payload.begin(), payload.end()));
}
} // namespace

Bytes detail::encode(const detail::State& state) {
    Bytes data;
    field(data, view(magic));
    encoding::u64(data, state.tables.size());
    for (const auto& [name, stored] : state.tables) {
        const auto& table = *stored;
        field(data, view(name));
        write_schema(data, table);
        encoding::u64(data, static_cast<std::uint64_t>(table.next_rowid));
        encoding::u64(data, table.row_count);
        for (const auto& [id, chunk] : table.chunks) {
            (void)id;
            for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
                encoding::u64(data, static_cast<std::uint64_t>(chunk->rowids[i]));
                for (const auto& value : chunk->rows[i])
                    write_value(data, value);
                if (data.size() > max_snapshot - 8)
                    throw Error(ErrorCode::format, "Snapshot exceeds configured encoded-size limit");
            }
        }
    }
    if (data.size() > max_snapshot - 8)
        throw Error(ErrorCode::format, "Snapshot exceeds configured encoded-size limit");
    encoding::u64(data, checksum(data));
    return data;
}

void Database::save(const std::filesystem::path& path) const {
    detail::healthy(owner_);
    auto data = detail::encode(*owner_->current);
    // Exclusive creation avoids replacing an existing database/export by mistake.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        throw Error(ErrorCode::io, "Cannot create snapshot: " + std::string(std::strerror(errno)));
    std::size_t offset = 0;
    while (offset < data.size()) {
        auto n = ::write(fd, data.data() + offset, data.size() - offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            int error = errno;
            ::close(fd);
            ::unlink(path.c_str());
            throw Error(ErrorCode::io, "Snapshot write failed: " + std::string(std::strerror(error)));
        }
        offset += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        ::unlink(path.c_str());
        throw Error(ErrorCode::io, "Snapshot close failed");
    }
}

Database Database::load(const std::filesystem::path& path, Registry registry) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw Error(ErrorCode::io, "Cannot open snapshot");
    auto size = file.tellg();
    if (size < 8 || size > static_cast<std::streamoff>(max_snapshot))
        throw Error(ErrorCode::format, "Invalid snapshot size (exceeds configured encoded-size limit)");
    Bytes data(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        throw Error(ErrorCode::io, "Cannot read snapshot");
    return decode(data, std::move(registry));
}

void detail::validate_snapshot(ByteView bytes) {
    if (bytes.size() < 8 || bytes.size() > max_snapshot)
        throw Error(ErrorCode::format, "Invalid snapshot size");
    Reader trailer(bytes.last(8));
    if (checksum(bytes.first(bytes.size() - 8)) != trailer.u64())
        throw Error(ErrorCode::format, "Snapshot checksum mismatch");
}

Database Database::decode(ByteView bytes, Registry registry) {
    detail::validate_snapshot(bytes);
    Reader reader(bytes.first(bytes.size() - 8));
    auto format = string(reader);
    if (format != magic && format != "CORESQL4" && format != "CORESQL3" && format != "CORESQL2" &&
        format != "CORESQL1")
        throw Error(ErrorCode::format, "Unsupported snapshot format/version");
    Database database(std::move(registry));
    auto transaction = database.begin();
    auto tables = count(reader);
    for (std::size_t t = 0; t < tables; ++t) {
        auto name = string(reader);
        auto schema = read_schema(reader, database.owner_->registry, format != "CORESQL1",
                                  format == "CORESQL3" || format == "CORESQL4" || format == magic,
                                  format == "CORESQL4" || format == magic, format == magic);
        const auto& columns = schema.columns;
        const auto ncolumns = columns.size();
        transaction.create_table(name, columns);
        for (const auto& d : schema.index_definitions)
            transaction.create_index(name, d);
        auto next_rowid = (format == magic || format == "CORESQL4") ? reader.u64() : 0;
        std::set<std::uint64_t> rowids;
        auto rows = count(reader);
        if (rows > reader.remaining() / 8 / ncolumns)
            throw Error(ErrorCode::format, "Invalid snapshot row count");
        for (std::size_t r = 0; r < rows; ++r) {
            auto rowid = (format == magic || format == "CORESQL4") ? reader.u64() : r + 1;
            if (!rowid || rowid >= INT64_MAX || !rowids.insert(rowid).second)
                throw Error(ErrorCode::format, "Invalid row identity");
            Row row;
            for (const auto& column : columns) {
                row.push_back(read_value(reader, column.type, database.owner_->registry, format == magic));
            }
            transaction.insert_impl(name, std::move(row), static_cast<std::int64_t>(rowid));
        }
        if (format == magic || format == "CORESQL4") {
            if (!next_rowid || next_rowid > INT64_MAX || (!rowids.empty() && next_rowid <= *rowids.rbegin()))
                throw Error(ErrorCode::format, "Invalid row identity sequence");
            transaction.restore_row_sequence(name, static_cast<std::int64_t>(next_rowid));
        }
    }
    reader.end();
    transaction.commit();
    return database;
}

// A change record replaces only changed chunks. Schemas are repeated for changed
// tables so validation remains independent of internal type-registration order.
std::size_t detail::checkpoint_size(const State& state) {
    std::size_t bytes = 32;
    for (const auto& [name, table] : state.tables) {
        bytes += 40 + name.size();
        Bytes schema;
        write_schema(schema, *table);
        bytes += schema.size();
        for (const auto& [id, chunk] : table->chunks) {
            (void)id;
            bytes += 16 + chunk->encoded_bytes;
        }
        if (bytes > max_snapshot)
            throw Error(ErrorCode::format, "Live encoded state exceeds configured encoded-size limit");
    }
    return bytes;
}
Bytes detail::encode_changes(const State& base, const State& next) {
    checkpoint_size(next);
    Bytes data;
    field(data, view("CORECHG6"));
    std::size_t changed = 0;
    for (const auto& [name, table] : next.tables) {
        auto old = base.tables.find(name);
        if (old == base.tables.end() || old->second != table)
            ++changed;
    }
    encoding::u64(data, changed);
    for (const auto& [name, table] : next.tables) {
        auto old = base.tables.find(name);
        if (old != base.tables.end() && old->second == table)
            continue;
        const Table empty;
        const auto& previous = old == base.tables.end() ? empty : *old->second;
        field(data, view(name));
        write_schema(data, *table);
        encoding::u64(data, table->next_chunk);
        encoding::u64(data, static_cast<std::uint64_t>(table->next_rowid));
        std::size_t chunks = 0;
        for (const auto& [id, chunk] : table->chunks) {
            auto before = previous.chunks.find(id);
            if (before == previous.chunks.end() || before->second != chunk)
                ++chunks;
        }
        for (const auto& [id, chunk] : previous.chunks) {
            (void)chunk;
            if (!table->chunks.contains(id))
                ++chunks;
        }
        encoding::u64(data, chunks);
        for (const auto& [id, chunk] : table->chunks) {
            auto before = previous.chunks.find(id);
            if (before != previous.chunks.end() && before->second == chunk)
                continue;
            encoding::u64(data, id);
            encoding::u64(data, chunk->rows.size());
            for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
                encoding::u64(data, static_cast<std::uint64_t>(chunk->rowids[i]));
                for (const auto& value : chunk->rows[i])
                    write_value(data, value);
            }
        }
        for (const auto& [id, chunk] : previous.chunks) {
            (void)chunk;
            if (!table->chunks.contains(id)) {
                encoding::u64(data, id);
                encoding::u64(data, 0);
            }
        }
    }
    if (data.size() > max_snapshot - 8)
        throw Error(ErrorCode::format, "Change record exceeds configured encoded-size limit");
    encoding::u64(data, checksum(data));
    return data;
}
detail::State detail::apply_changes(const State& base, ByteView bytes, const Registry& registry) {
    validate_snapshot(bytes);
    Reader reader(bytes.first(bytes.size() - 8));
    auto format = string(reader);
    if (format != "CORECHG2" && format != "CORECHG3" && format != "CORECHG4" && format != "CORECHG5" &&
        format != "CORECHG6")
        throw Error(ErrorCode::format, "Unsupported change record");
    State next = base;
    std::set<std::string> seen;
    auto tables = count(reader);
    for (std::size_t t = 0; t < tables; ++t) {
        auto name = string(reader);
        if (name.empty() || !seen.insert(name).second)
            throw Error(ErrorCode::format, "Invalid changed table name");
        auto table = std::make_shared<Table>();
        auto old = base.tables.find(name);
        if (old != base.tables.end())
            *table = *old->second;
        auto declarations = read_schema(reader, registry, format != "CORECHG2",
                                        format == "CORECHG4" || format == "CORECHG5" || format == "CORECHG6",
                                        format == "CORECHG5" || format == "CORECHG6", format == "CORECHG6");
        const auto columns = declarations.columns.size();
        if (old != base.tables.end()) {
            if (declarations.columns.size() < table->columns.size() ||
                !std::equal(table->columns.begin(), table->columns.end(), declarations.columns.begin()))
                throw Error(ErrorCode::format, "Existing column changed in log");
            if (declarations.columns.size() != table->columns.size() && format != "CORECHG5" &&
                format != "CORECHG6")
                throw Error(ErrorCode::format, "Schema changed in legacy log");
        }
        table->columns = std::move(declarations.columns);
        table->index_definitions = std::move(declarations.index_definitions);
        auto next_id = reader.u64();
        if (next_id < table->next_chunk)
            throw Error(ErrorCode::format, "Chunk sequence moved backwards");
        table->next_chunk = next_id;
        if (format == "CORECHG5" || format == "CORECHG6") {
            auto next_rowid = reader.u64();
            if (next_rowid < static_cast<std::uint64_t>(table->next_rowid) || next_rowid > INT64_MAX)
                throw Error(ErrorCode::format, "Invalid row sequence");
            table->next_rowid = static_cast<std::int64_t>(next_rowid);
        }
        auto chunks = count(reader);
        std::set<std::uint64_t> ids;
        for (std::size_t c = 0; c < chunks; ++c) {
            auto id = reader.u64(), rows = reader.u64();
            if (id >= next_id || !ids.insert(id).second || rows > chunk_rows ||
                rows > reader.remaining() / 8 / columns)
                throw Error(ErrorCode::format, "Invalid chunk descriptor");
            if (!rows) {
                if (!table->chunks.erase(id))
                    throw Error(ErrorCode::format, "Deleting absent chunk");
                continue;
            }
            auto chunk = std::make_shared<Chunk>();
            for (std::uint64_t r = 0; r < rows; ++r) {
                auto rowid = (format == "CORECHG5" || format == "CORECHG6")
                                 ? reader.u64()
                                 : static_cast<std::uint64_t>(table->next_rowid++);
                if (!rowid || rowid >= static_cast<std::uint64_t>(table->next_rowid))
                    throw Error(ErrorCode::format, "Invalid row identity");
                chunk->rowids.push_back(static_cast<std::int64_t>(rowid));
                Row row;
                for (const auto& column : table->columns) {
                    row.push_back(read_value(reader, column.type, registry, format == "CORECHG6"));
                    detail::validate_stored(row.back(), column, registry);
                }
                chunk->rows.push_back(std::move(row));
            }
            refresh(*chunk);
            table->chunks[id] = std::move(chunk);
        }
        std::set<std::int64_t> identities;
        for (const auto& [id, chunk] : table->chunks) {
            (void)id;
            for (auto identity : chunk->rowids)
                if (!identities.insert(identity).second)
                    throw Error(ErrorCode::format, "Duplicate row identity");
            for (const auto& row : chunk->rows)
                if (row.size() != columns)
                    throw Error(ErrorCode::format, "Schema change omitted existing rows");
        }
        refresh(*table);
        next.tables[name] = std::move(table);
    }
    reader.end();
    checkpoint_size(next);
    next.stats = measure(next);
    return next;
}
} // namespace coresql
