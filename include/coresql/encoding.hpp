#pragma once
#include "core.hpp"
#include <bit>

namespace coresql::encoding {
// Portable little-endian primitives, also usable by add-ons. No native structs
// are persisted, and no unaligned typed loads are performed.
inline void u64(Bytes& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(std::byte((value >> (8 * i)) & 255));
}
inline void u32(Bytes& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(std::byte((value >> (8 * i)) & 255));
}
class Reader {
public:
    explicit Reader(ByteView bytes) : bytes_(bytes) {}
    std::size_t remaining() const { return bytes_.size() - position_; }
    ByteView take(std::size_t count) {
        if (count > remaining()) throw Error(ErrorCode::format, "Truncated encoded value");
        auto result = bytes_.subspan(position_, count);
        position_ += count;
        return result;
    }
    std::uint64_t u64() {
        auto data = take(8);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= std::uint64_t(std::to_integer<unsigned>(data[i])) << (8 * i);
        return value;
    }
    std::uint32_t u32() {
        auto data = take(4);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(std::to_integer<unsigned>(data[i])) << (8 * i);
        return value;
    }
    void end() const {
        if (remaining()) throw Error(ErrorCode::format, "Unexpected trailing bytes");
    }
private:
    ByteView bytes_;
    std::size_t position_ = 0;
};
} // namespace coresql::encoding
