#include "coresql/core.hpp"
#include "coresql/encoding.hpp"
#include "storage.hpp"
#include "index.hpp"
#include "pager.hpp"

#include <array>
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
constexpr std::string_view magic = "CORESQL6";
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
void write_expr(Bytes& data, const Expr& e) {
    encoding::u64(data, static_cast<std::uint64_t>(e.kind));
    field(data, view(e.name));
    if (e.kind == Expr::Kind::literal) {
        const auto type = type_of(e.value);
        field(data, view(type.id));
        encoding::u64(data, type.version);
        field(data, type.parameters);
        write_value(data, e.value);
    }
    encoding::u64(data, e.arguments.size());
    for (const auto& argument : e.arguments)
        write_expr(data, argument);
}
Expr read_expr(Reader& reader, const Registry& registry, std::size_t& nodes, unsigned depth = 0) {
    if (++nodes > 4096 || depth > 64)
        throw Error(ErrorCode::format, "CHECK expression is too large");
    const auto kind = reader.u64();
    if (kind > static_cast<std::uint64_t>(Expr::Kind::membership))
        throw Error(ErrorCode::format, "Invalid CHECK expression kind");
    Expr e{static_cast<Expr::Kind>(kind), string(reader), std::int64_t{0}, {}};
    if (e.kind == Expr::Kind::literal) {
        auto id = string(reader);
        auto version = reader.u64();
        auto parameters = field(reader);
        if (!version || version > UINT32_MAX)
            throw Error(ErrorCode::format, "Invalid literal type version");
        Type type{std::move(id), static_cast<std::uint32_t>(version),
                  Bytes(parameters.begin(), parameters.end())};
        registry.validate(type);
        e.value = read_value(reader, type, registry, true);
        registry.validate(e.value, type);
    }
    const auto width = count(reader);
    if (width > 4096 - nodes)
        throw Error(ErrorCode::format, "CHECK expression is too large");
    for (std::size_t i = 0; i < width; ++i)
        e.arguments.push_back(read_expr(reader, registry, nodes, depth + 1));
    return e;
}
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
    encoding::u64(data, table.constraints.checks.size());
    for (const auto& check : table.constraints.checks) {
        field(data, view(check.name));
        detail::validate_check_expression(check.expression);
        write_expr(data, check.expression);
    }
    encoding::u64(data, table.constraints.foreign_keys.size());
    for (const auto& key : table.constraints.foreign_keys) {
        field(data, view(key.name));
        field(data, view(key.referenced_table));
        encoding::u64(data, key.columns.size());
        for (const auto& c : key.columns)
            field(data, view(c));
        encoding::u64(data, key.referenced_columns.size());
        for (const auto& c : key.referenced_columns)
            field(data, view(c));
    }
}
detail::Table read_schema(Reader& reader, const Registry& registry, bool primary, bool indexes, bool extended,
                          bool tagged, bool constrained) {
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
    if (constrained) {
        const auto nchecks = count(reader);
        for (std::size_t i = 0; i < nchecks; ++i) {
            auto name = string(reader);
            std::size_t nodes = 0;
            auto expression = read_expr(reader, registry, nodes);
            detail::validate_check_expression(expression);
            table.constraints.checks.push_back({std::move(name), std::move(expression)});
        }
        const auto nkeys = count(reader);
        for (std::size_t i = 0; i < nkeys; ++i) {
            ForeignKey key;
            key.name = string(reader);
            key.referenced_table = string(reader);
            const auto ncolumns = count(reader);
            for (std::size_t j = 0; j < ncolumns; ++j)
                key.columns.push_back(string(reader));
            const auto nparent = count(reader);
            for (std::size_t j = 0; j < nparent; ++j)
                key.referenced_columns.push_back(string(reader));
            table.constraints.foreign_keys.push_back(std::move(key));
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
    else if (const auto* cell = std::get_if<Compact>(&value)) {
        if (cell->bytes().size() == 8) {
            Bytes payload;
            encoding::u64(payload, std::bit_cast<std::uint64_t>(i64_payload(value)));
            field(data, payload);
        } else
            field(data, cell->bytes());
    } else
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
    if (layout == Layout::i64) {
        if (type == integer())
            return std::bit_cast<std::int64_t>(reader.u64());
        auto bytes = field(reader);
        if (bytes.size() != 8)
            throw Error(ErrorCode::format, "Invalid compact i64 payload");
        encoding::Reader payload(bytes);
        auto value = std::bit_cast<std::int64_t>(payload.u64());
        payload.end();
        return compact(type, value);
    }
    if (layout == Layout::f64)
        return std::bit_cast<double>(reader.u64());
    if (layout == Layout::text)
        return string(reader);
    if (layout == Layout::i128) {
        auto bytes = field(reader);
        if (bytes.size() != 16)
            throw Error(ErrorCode::format, "Invalid compact i128 payload");
        std::array<std::byte, 16> payload{};
        std::memcpy(payload.data(), bytes.data(), 16);
        return compact(type, payload);
    }
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
        for (const auto& [id, reference] : table.chunks) {
            (void)id;
            const auto chunk = reference.pin();
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
    auto data = detail::encode(*owner_->capture());
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
    if (format != magic && format != "CORESQL5" && format != "CORESQL4" && format != "CORESQL3" &&
        format != "CORESQL2" && format != "CORESQL1")
        throw Error(ErrorCode::format, "Unsupported snapshot format/version");
    Database database(std::move(registry));
    auto transaction = database.begin();
    auto tables = count(reader);
    std::map<std::string, TableConstraints> constraints;
    const bool tagged = format == magic || format == "CORESQL5";
    const bool extended = tagged || format == "CORESQL4";
    for (std::size_t t = 0; t < tables; ++t) {
        auto name = string(reader);
        auto schema = read_schema(reader, database.owner_->registry, format != "CORESQL1",
                                  format == "CORESQL3" || extended, extended, tagged, format == magic);
        constraints.emplace(name, std::move(schema.constraints));
        const auto& columns = schema.columns;
        const auto ncolumns = columns.size();
        transaction.create_table(name, columns);
        for (const auto& d : schema.index_definitions)
            transaction.create_index(name, d);
        auto next_rowid = extended ? reader.u64() : 0;
        std::set<std::uint64_t> rowids;
        auto rows = count(reader);
        if (rows > reader.remaining() / 8 / ncolumns)
            throw Error(ErrorCode::format, "Invalid snapshot row count");
        for (std::size_t r = 0; r < rows; ++r) {
            auto rowid = extended ? reader.u64() : r + 1;
            if (!rowid || rowid >= INT64_MAX || !rowids.insert(rowid).second)
                throw Error(ErrorCode::format, "Invalid row identity");
            Row row;
            for (const auto& column : columns) {
                row.push_back(read_value(reader, column.type, database.owner_->registry, tagged));
            }
            transaction.insert_impl(name, std::move(row), static_cast<std::int64_t>(rowid));
        }
        if (extended) {
            if (!next_rowid || next_rowid > INT64_MAX || (!rowids.empty() && next_rowid <= *rowids.rbegin()))
                throw Error(ErrorCode::format, "Invalid row identity sequence");
            transaction.restore_row_sequence(name, static_cast<std::int64_t>(next_rowid));
        }
    }
    reader.end();
    for (auto& [name, declarations] : constraints)
        transaction.set_constraints(name, std::move(declarations));
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
            bytes += 16 + chunk.encoded_bytes();
        }
        if (bytes > max_snapshot)
            throw Error(ErrorCode::format, "Live encoded state exceeds configured encoded-size limit");
    }
    return bytes;
}
Bytes detail::encode_changes(const State& base, const State& next) {
    checkpoint_size(next);
    Bytes data;
    field(data, view("CORECHG7"));
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
            const auto pinned = chunk.pin();
            encoding::u64(data, id);
            encoding::u64(data, pinned->rows.size());
            for (std::size_t i = 0; i < pinned->rows.size(); ++i) {
                encoding::u64(data, static_cast<std::uint64_t>(pinned->rowids[i]));
                for (const auto& value : pinned->rows[i])
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
// Checkpoints can be much larger than the working page cache. Encode scalars into
// a small buffer and pass large value payloads directly to the file sink. Only a
// table's schema is temporarily materialized; rows and chunks are never copied.
void detail::encode_checkpoint(const State& state, const std::function<void(ByteView)>& sink) {
    checkpoint_size(state);
    std::uint64_t hash = 14695981039346656037ULL;
    std::size_t size = 0;
    Bytes buffer;
    buffer.reserve(64 * 1024);
    auto flush = [&] {
        if (!buffer.empty()) {
            sink(buffer);
            buffer.clear();
        }
    };
    auto emit = [&](ByteView bytes) {
        if (bytes.size() > max_snapshot - 8 || size > max_snapshot - 8 - bytes.size())
            throw Error(ErrorCode::format, "Checkpoint exceeds configured encoded-size limit");
        size += bytes.size();
        for (auto byte : bytes) {
            hash ^= std::to_integer<unsigned>(byte);
            hash *= 1099511628211ULL;
        }
        if (buffer.size() + bytes.size() > 64 * 1024)
            flush();
        if (bytes.size() >= 64 * 1024)
            sink(bytes);
        else
            buffer.insert(buffer.end(), bytes.begin(), bytes.end());
    };
    auto number = [&](std::uint64_t value) {
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i < 8; ++i)
            bytes[i] = std::byte((value >> (8 * i)) & 255);
        emit(bytes);
    };
    auto bytes_field = [&](ByteView bytes) {
        number(bytes.size());
        emit(bytes);
    };
    auto value = [&](const Value& item) {
        number(is_null(item));
        if (is_null(item))
            return;
        if (const auto* i = std::get_if<std::int64_t>(&item))
            number(std::bit_cast<std::uint64_t>(*i));
        else if (const auto* d = std::get_if<double>(&item))
            number(std::bit_cast<std::uint64_t>(*d));
        else if (const auto* text = std::get_if<std::string>(&item))
            bytes_field(view(*text));
        else if (const auto* cell = std::get_if<Compact>(&item)) {
            if (cell->bytes().size() == 8) {
                number(8);
                number(std::bit_cast<std::uint64_t>(i64_payload(item)));
            } else
                bytes_field(cell->bytes());
        } else
            bytes_field(std::get<Opaque>(item).bytes());
    };
    bytes_field(view("CORECHG7"));
    number(state.tables.size());
    for (const auto& [name, table] : state.tables) {
        bytes_field(view(name));
        Bytes schema;
        write_schema(schema, *table);
        emit(schema);
        number(table->next_chunk);
        number(static_cast<std::uint64_t>(table->next_rowid));
        number(table->chunks.size());
        for (const auto& [id, reference] : table->chunks) {
            const auto chunk = reference.pin();
            number(id);
            number(chunk->rows.size());
            for (std::size_t row = 0; row < chunk->rows.size(); ++row) {
                number(static_cast<std::uint64_t>(chunk->rowids[row]));
                for (const auto& item : chunk->rows[row])
                    value(item);
            }
        }
    }
    flush();
    Bytes trailer;
    encoding::u64(trailer, hash);
    sink(trailer);
}

detail::State detail::apply_changes(const State& base, ByteView bytes, const Registry& registry,
                                    const std::shared_ptr<Pager>& pager) {
    validate_snapshot(bytes);
    Reader reader(bytes.first(bytes.size() - 8));
    auto format = string(reader);
    if (format != "CORECHG2" && format != "CORECHG3" && format != "CORECHG4" && format != "CORECHG5" &&
        format != "CORECHG6" && format != "CORECHG7")
        throw Error(ErrorCode::format, "Unsupported change record");
    const bool tagged = format == "CORECHG6" || format == "CORECHG7";
    const bool extended = tagged || format == "CORECHG5";
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
        auto declarations =
            read_schema(reader, registry, format != "CORECHG2", format == "CORECHG4" || extended, extended,
                        tagged, format == "CORECHG7");
        const auto columns = declarations.columns.size();
        if (old != base.tables.end()) {
            if (declarations.columns.size() < table->columns.size() ||
                !std::equal(table->columns.begin(), table->columns.end(), declarations.columns.begin()))
                throw Error(ErrorCode::format, "Existing column changed in log");
            if (declarations.columns.size() != table->columns.size() && !extended)
                throw Error(ErrorCode::format, "Schema changed in legacy log");
        }
        table->constraints = std::move(declarations.constraints);
        table->columns = std::move(declarations.columns);
        table->index_definitions = std::move(declarations.index_definitions);
        auto next_id = reader.u64();
        if (next_id < table->next_chunk)
            throw Error(ErrorCode::format, "Chunk sequence moved backwards");
        table->next_chunk = next_id;
        if (extended) {
            auto next_rowid = reader.u64();
            if (next_rowid < static_cast<std::uint64_t>(table->next_rowid) || next_rowid > INT64_MAX)
                throw Error(ErrorCode::format, "Invalid row sequence");
            table->next_rowid = static_cast<std::int64_t>(next_rowid);
        }
        auto chunks = count(reader);
        auto page_columns = pager ? std::make_shared<const std::vector<Column>>(table->columns) : nullptr;
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
                auto rowid = (extended) ? reader.u64() : static_cast<std::uint64_t>(table->next_rowid++);
                if (!rowid || rowid >= static_cast<std::uint64_t>(table->next_rowid))
                    throw Error(ErrorCode::format, "Invalid row identity");
                chunk->rowids.push_back(static_cast<std::int64_t>(rowid));
                Row row;
                for (const auto& column : table->columns) {
                    row.push_back(read_value(reader, column.type, registry, tagged));
                    detail::validate_stored(row.back(), column, registry);
                }
                chunk->rows.push_back(std::move(row));
            }
            refresh(*chunk);
            table->chunks[id] =
                pager ? pager->store(std::move(chunk), page_columns) : ChunkRef(std::move(chunk));
        }
        std::set<std::int64_t> identities;
        for (const auto& [id, reference] : table->chunks) {
            (void)id;
            const auto chunk = reference.pin();
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
Bytes detail::encode_page(const Chunk& chunk) {
    Bytes data;
    encoding::u64(data, chunk.rows.size());
    encoding::u64(data, chunk.slots.size());
    for (auto slot : chunk.slots)
        encoding::u64(data, slot);
    for (std::size_t i = 0; i < chunk.rows.size(); ++i) {
        encoding::u64(data, static_cast<std::uint64_t>(chunk.rowids[i]));
        for (const auto& value : chunk.rows[i])
            write_value(data, value);
    }
    encoding::u64(data, checksum(data));
    return data;
}
std::shared_ptr<detail::Chunk> detail::decode_page(ByteView data, const std::vector<Column>& columns,
                                                   const Registry& registry) {
    validate_snapshot(data);
    Reader reader(data.first(data.size() - 8));
    auto rows = reader.u64(), slots = reader.u64();
    if (rows > chunk_rows || (slots && slots != rows))
        throw Error(ErrorCode::format, "Invalid cached chunk");
    auto chunk = std::make_shared<Chunk>();
    for (std::size_t i = 0; i < slots; ++i) {
        const auto slot = reader.u64();
        if (slot > UINT32_MAX || (!chunk->slots.empty() && slot <= chunk->slots.back()))
            throw Error(ErrorCode::format, "Invalid cached slots");
        chunk->slots.push_back(static_cast<std::uint32_t>(slot));
    }
    chunk->rows.reserve(rows);
    chunk->rowids.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        chunk->rowids.push_back(static_cast<std::int64_t>(reader.u64()));
        Row row;
        row.reserve(columns.size());
        for (const auto& column : columns)
            row.push_back(read_value(reader, column.type, registry, true));
        chunk->rows.push_back(std::move(row));
    }
    reader.end();
    refresh(*chunk);
    return chunk;
}
} // namespace coresql
