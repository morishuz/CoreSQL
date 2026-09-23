#include "paged_index.hpp"
#include "index.hpp"
#include "coresql/encoding.hpp"
#include "query_control.hpp"
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <queue>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace coresql::detail {
namespace {
constexpr std::size_t record_size = 24, run_size = 4096;
constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
std::uint64_t checksum(ByteView bytes, std::uint64_t hash = fnv_offset) {
    for (auto byte : bytes) {
        hash ^= std::to_integer<unsigned>(byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}
[[noreturn]] void io(const char* what) {
    throw Error(ErrorCode::io, std::string(what) + ": " + std::strerror(errno));
}
class File {
    int fd_ = -1;
    const std::byte* mapped_ = nullptr;
    std::size_t size_ = 0;

public:
    File() {
        auto pattern = (std::filesystem::temp_directory_path() / "coresql-index-XXXXXX").string();
        fd_ = ::mkstemp(pattern.data());
        if (fd_ < 0)
            io("Create index backing");
        if (::unlink(pattern.c_str()) < 0 || ::fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) {
            const auto error = errno;
            ::close(fd_);
            fd_ = -1;
            errno = error;
            io("Prepare index backing");
        }
    }
    explicit File(const std::filesystem::path& path) {
        fd_ = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd_ < 0)
            io("Open index cache");
        struct stat status{};
        if (::fstat(fd_, &status) < 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
            ::close(fd_);
            fd_ = -1;
            throw Error(ErrorCode::format, "Invalid index cache file");
        }
        size_ = static_cast<std::size_t>(status.st_size);
    }
    ~File() {
        if (mapped_)
            ::munmap(const_cast<std::byte*>(mapped_), size_);
        if (fd_ >= 0)
            ::close(fd_);
    }
    void append(ByteView bytes) {
        if (mapped_)
            throw Error(ErrorCode::state, "Cannot append mapped index");
        while (!bytes.empty()) {
            auto n = ::write(fd_, bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                io("Write index backing");
            size_ += static_cast<std::size_t>(n);
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
    }
    ByteView map() {
        if (!size_)
            return {};
        if (!mapped_) {
            auto* address = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (address == MAP_FAILED)
                io("Map index backing");
            mapped_ = static_cast<const std::byte*>(address);
        }
        return {mapped_, size_};
    }
    ByteView bytes() const {
        if (size_ && !mapped_)
            throw Error(ErrorCode::state, "Index image was not mapped before publication");
        return {mapped_, size_};
    }
};
struct Key {
    std::int64_t key;
    RowLocation location;
};
Key read_key(ByteView bytes, std::size_t position) {
    encoding::Reader reader(bytes.subspan(position * record_size, record_size));
    const auto key = std::bit_cast<std::int64_t>(reader.u64());
    const auto chunk = reader.u64(), slot = reader.u64();
    if (slot > UINT32_MAX)
        throw Error(ErrorCode::format, "Invalid cached index slot");
    return {key, {chunk, static_cast<std::uint32_t>(slot)}};
}
void write_key(Bytes& bytes, Key entry) {
    encoding::u64(bytes, std::bit_cast<std::uint64_t>(entry.key));
    encoding::u64(bytes, entry.location.chunk);
    encoding::u64(bytes, entry.location.slot);
}
struct Base {
    std::shared_ptr<File> file;
    std::size_t offset = 0, count = 0;
    ByteView bytes() const { return file->bytes().subspan(offset, count * record_size); }
    Key get(std::size_t i) const { return read_key(bytes(), i); }
    std::size_t lower_bound(std::int64_t key) const {
        std::size_t first = 0, last = count;
        while (first < last) {
            const auto middle = first + (last - first) / 2;
            const auto row = get(middle);
            if (row.key < key)
                first = middle + 1;
            else
                last = middle;
        }
        return first;
    }
    std::optional<RowLocation> lookup(std::int64_t key) const {
        const auto first = lower_bound(key);
        if (first != count) {
            const auto row = get(first);
            if (row.key == key)
                return row.location;
        }
        return {};
    }
};
class Writer {
    std::shared_ptr<File> file_ = std::make_shared<File>();
    Bytes buffer_;
    std::size_t count_ = 0;
    std::optional<std::int64_t> previous_;

public:
    void add(Key key) {
        if (previous_ && *previous_ >= key.key)
            throw Error(ErrorCode::constraint, "Duplicate primary key");
        previous_ = key.key;
        write_key(buffer_, key);
        ++count_;
        if (buffer_.size() >= 64 * 1024) {
            file_->append(buffer_);
            buffer_.clear();
        }
    }
    Base finish() {
        file_->append(buffer_);
        file_->map();
        return {std::move(file_), 0, count_};
    }
};
// Only one bounded run is in the heap. Runs and merge output live on disk; mmap
// pages are reclaimable by the OS, outside the decoded-row cache target.
Base build(const Table& table, std::size_t column) {
    auto runs_file = std::make_shared<File>();
    std::vector<Key> buffer;
    buffer.reserve(run_size);
    struct Run {
        std::size_t begin, count;
    };
    std::vector<Run> runs;
    std::size_t total = 0;
    auto flush = [&] {
        if (buffer.empty())
            return;
        std::sort(buffer.begin(), buffer.end(), [](auto a, auto b) { return a.key < b.key; });
        Bytes bytes;
        bytes.reserve(buffer.size() * record_size);
        for (auto entry : buffer)
            write_key(bytes, entry);
        runs_file->append(bytes);
        runs.push_back({total, buffer.size()});
        total += buffer.size();
        buffer.clear();
    };
    for (const auto& [id, reference] : table.chunks) {
        const auto chunk = reference.pin();
        for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
            buffer.push_back({std::get<std::int64_t>(chunk->rows[i][column]), {id, chunk->slot(i)}});
            if (buffer.size() == run_size)
                flush();
        }
    }
    flush();
    const auto bytes = runs_file->map();
    struct Head {
        Key key;
        std::size_t run, position;
    };
    auto compare = [](const Head& a, const Head& b) { return a.key.key > b.key.key; };
    std::priority_queue<Head, std::vector<Head>, decltype(compare)> queue(compare);
    for (std::size_t i = 0; i < runs.size(); ++i)
        queue.push({read_key(bytes, runs[i].begin), i, 0});
    Writer output;
    while (!queue.empty()) {
        auto entry = queue.top();
        queue.pop();
        output.add(entry.key);
        if (++entry.position < runs[entry.run].count) {
            entry.key = read_key(bytes, runs[entry.run].begin + entry.position);
            queue.push(entry);
        }
    }
    return output.finish();
}
class PagedIntegerIndex final : public Index {
    Base base_;
    using Changes = std::map<std::int64_t, std::optional<RowLocation>>;
    std::shared_ptr<Changes> changes_ = std::make_shared<Changes>();
    void compact() {
        Writer output;
        std::size_t position = 0;
        auto change = changes_->begin();
        while (position < base_.count || change != changes_->end()) {
            if (position == base_.count ||
                (change != changes_->end() && change->first <= base_.get(position).key)) {
                if (position < base_.count && change->first == base_.get(position).key)
                    ++position;
                if (change->second)
                    output.add({change->first, *change->second});
                ++change;
            } else
                output.add(base_.get(position++));
        }
        auto next = output.finish();
        auto empty = std::make_shared<Changes>();
        base_ = std::move(next);
        changes_ = std::move(empty);
    }
    Changes& change() {
        if (changes_->size() >= run_size)
            compact();
        if (changes_.use_count() != 1)
            changes_ = std::make_shared<Changes>(*changes_);
        return *changes_;
    }

public:
    explicit PagedIntegerIndex(Base base) : base_(std::move(base)) {}
    const Base& base() const { return base_; }
    std::shared_ptr<Index> clone() const override { return std::make_shared<PagedIntegerIndex>(*this); }
    std::optional<RowLocation> lookup(const Value& value) const override {
        const auto key = std::get<std::int64_t>(value);
        auto found = changes_->find(key);
        return found != changes_->end() ? found->second : base_.lookup(key);
    }
    std::optional<IndexResult> range(const Value* lower, const Value* upper) const override {
        const auto lo = lower ? std::get<std::int64_t>(*lower) : INT64_MIN;
        const auto hi = upper ? std::get<std::int64_t>(*upper) : INT64_MAX;
        IndexResult result;
        auto position = base_.lower_bound(lo);
        auto change = changes_->lower_bound(lo);
        std::optional<Key> base;
        auto advance = [&] {
            base = position < base_.count ? std::optional(base_.get(position++)) : std::nullopt;
            if (base && base->key > hi)
                base.reset();
        };
        advance();
        while (base || (change != changes_->end() && change->first <= hi)) {
            execution::query_step();
            if (change != changes_->end() && change->first <= hi && (!base || change->first <= base->key)) {
                if (base && change->first == base->key)
                    advance();
                if (change->second)
                    result.rows.push_back(*change->second);
                ++change;
            } else {
                result.rows.push_back(base->location);
                advance();
            }
        }
        return result;
    }
    void insert(const Value& value, RowLocation location) override {
        if (lookup(value))
            throw Error(ErrorCode::constraint, "Duplicate primary key");
        change().insert_or_assign(std::get<std::int64_t>(value), location);
    }
    void erase(const Value& value, RowLocation location) override {
        const auto found = lookup(value);
        if (!found || *found != location)
            throw Error(ErrorCode::state, "Missing primary index entry");
        change().insert_or_assign(std::get<std::int64_t>(value), std::nullopt);
    }
    void validate(std::span<const IndexEntry> rows) const override {
        auto count = base_.count;
        for (const auto& [key, location] : *changes_) {
            if (base_.lookup(key))
                --count;
            if (location)
                ++count;
        }
        if (count != rows.size())
            throw Error(ErrorCode::state, "Paged index cardinality mismatch");
        for (const auto& row : rows)
            if (lookup(row.value) != row.location)
                throw Error(ErrorCode::state, "Paged index mismatch");
    }
};
bool eligible(const Table& table, const Registry& registry) {
    if (!table.primary)
        return false;
    const auto& column = table.columns[table.primary->column];
    return column.type == integer() && (column.index.empty() || column.index == "core.hash") &&
           registry.addon(column.type).native_ops;
}
using Images = std::map<std::string, Base>;
Images load_cache(const std::filesystem::path& path, const State& state, const Registry& registry,
                  std::uint64_t fingerprint) {
    auto file = std::make_shared<File>(path);
    const auto bytes = file->map();
    if (bytes.size() < 32)
        throw Error(ErrorCode::format, "Short index cache");
    encoding::Reader trailer(bytes.last(8));
    if (checksum(bytes.first(bytes.size() - 8)) != trailer.u64())
        throw Error(ErrorCode::format, "Index cache checksum mismatch");
    encoding::Reader reader(bytes.first(bytes.size() - 8));
    const auto magic = reader.take(8);
    if (std::memcmp(magic.data(), "COREIDX1", 8) || reader.u64() != fingerprint)
        throw Error(ErrorCode::format, "Stale index cache");
    auto tables = reader.u64();
    if (tables > state.tables.size())
        throw Error(ErrorCode::format, "Invalid index cache count");
    Images images;
    for (std::size_t i = 0; i < tables; ++i) {
        const auto length = reader.u64();
        if (length > reader.remaining())
            throw Error(ErrorCode::format, "Invalid index cache name");
        const auto name_bytes = reader.take(length);
        std::string name(reinterpret_cast<const char*>(name_bytes.data()), name_bytes.size());
        const auto column = reader.u64(), count = reader.u64();
        auto table = state.tables.find(name);
        if (table == state.tables.end() || !eligible(*table->second, registry) ||
            table->second->primary->column != column || table->second->row_count != count ||
            count > reader.remaining() / record_size)
            throw Error(ErrorCode::format, "Invalid cached index descriptor");
        const auto payload = reader.take(count * record_size);
        Base image{file, static_cast<std::size_t>(payload.data() - bytes.data()),
                   static_cast<std::size_t>(count)};
        std::optional<std::int64_t> previous;
        for (std::size_t row = 0; row < count; ++row) {
            const auto key = image.get(row);
            const auto chunk = table->second->chunks.find(key.location.chunk);
            if ((previous && *previous >= key.key) || chunk == table->second->chunks.end() ||
                key.location.slot >= chunk->second.rows())
                throw Error(ErrorCode::format, "Invalid cached index entry");
            previous = key.key;
        }
        // The cache is an optimization, not a second source of truth. Verify
        // every mapping against validated durable rows, including forged images
        // whose checksum/fingerprint fields happen to be internally consistent.
        for (const auto& [id, reference] : table->second->chunks) {
            const auto chunk = reference.pin();
            for (std::size_t row = 0; row < chunk->rows.size(); ++row)
                if (image.lookup(std::get<std::int64_t>(chunk->rows[row][column])) !=
                    RowLocation{id, chunk->slot(row)})
                    throw Error(ErrorCode::format, "Cached index disagrees with stored rows");
        }
        if (!images.emplace(std::move(name), std::move(image)).second)
            throw Error(ErrorCode::format, "Duplicate cached table");
    }
    reader.end();
    return images;
}
void write_cache(const std::filesystem::path& path, const Images& images, const State& state,
                 std::uint64_t fingerprint) {
    auto temporary = path.string() + ".tmp-XXXXXX";
    int fd = ::mkstemp(temporary.data());
    if (fd < 0)
        io("Create index cache");
    auto hash = fnv_offset;
    auto write = [&](ByteView bytes, bool include = true) {
        if (include)
            hash = checksum(bytes, hash);
        while (!bytes.empty()) {
            const auto n = ::write(fd, bytes.data(), bytes.size());
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                io("Write index cache");
            bytes = bytes.subspan(static_cast<std::size_t>(n));
        }
    };
    try {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
            io("Prepare index cache");
        Bytes header;
        for (char c : std::string_view("COREIDX1"))
            header.push_back(static_cast<std::byte>(c));
        encoding::u64(header, fingerprint);
        encoding::u64(header, images.size());
        write(header);
        for (const auto& [name, image] : images) {
            Bytes descriptor;
            encoding::u64(descriptor, name.size());
            for (char c : name)
                descriptor.push_back(static_cast<std::byte>(c));
            encoding::u64(descriptor, state.tables.at(name)->primary->column);
            encoding::u64(descriptor, image.count);
            write(descriptor);
            write(image.bytes());
        }
        Bytes trailer;
        encoding::u64(trailer, hash);
        write(trailer, false);
        const auto closed = ::close(fd);
        fd = -1;
        if (closed < 0)
            io("Close index cache");
        if (::rename(temporary.c_str(), path.c_str()) < 0)
            io("Publish index cache");
        // Cache durability is unnecessary: missing, stale or truncated files rebuild.
    } catch (...) {
        if (fd >= 0)
            ::close(fd);
        ::unlink(temporary.c_str());
        throw;
    }
}
} // namespace
void page_primary_index(Table& table, const Registry& registry) {
    if (!eligible(table, registry) || dynamic_cast<PagedIntegerIndex*>(table.primary->data.get()))
        return;
    const auto column = table.primary->column;
    auto index = std::make_shared<PagedIntegerIndex>(build(table, column));
    table.primary = std::make_shared<IndexBinding>(IndexBinding{column, std::move(index)});
}
void rebuild_paged_indexes(State& state, const Registry& registry, const std::filesystem::path& database,
                           std::uint64_t fingerprint) {
    for (auto& [name, table] : state.tables) {
        (void)name;
        make_indexes(*table, registry);
    }
    const auto path = std::filesystem::path(database.string() + ".native-index");
    Images images;
    try {
        images = load_cache(path, state, registry, fingerprint);
    } catch (const Error&) {
    }
    bool changed = false;
    for (auto& [name, table] : state.tables) {
        const bool native = eligible(*table, registry);
        if (native) {
            auto found = images.find(name);
            if (found == images.end()) {
                images.emplace(name, build(*table, table->primary->column));
                changed = true;
            }
            table->primary = std::make_shared<IndexBinding>(
                IndexBinding{table->primary->column, std::make_shared<PagedIntegerIndex>(images.at(name))});
        }
        if (table->ordered.empty() && table->indexes.empty() && (native || !table->primary))
            continue;
        for (const auto& [id, reference] : table->chunks) {
            const auto chunk = reference.pin();
            for (std::size_t i = 0; i < chunk->rows.size(); ++i) {
                const auto& row = chunk->rows[i];
                const RowLocation location{id, chunk->slot(i)};
                for (auto& index : table->ordered)
                    index->insert(row, location);
                if (table->primary && !native)
                    table->primary->data->insert(row[table->primary->column], location);
                for (auto& index : table->indexes)
                    index->data->insert(row[index->column], location);
            }
        }
    }
    if (changed)
        try {
            write_cache(path, images, state, fingerprint);
        } catch (const Error&) {
        }
}
} // namespace coresql::detail
