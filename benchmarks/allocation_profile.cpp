#include "allocation_profile.hpp"
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <new>
#include <cstddef>

namespace {
bool active = false;
std::size_t calls = 0, bytes = 0, small = 0, medium = 0, large = 0;
std::clock_t start;
void record(std::size_t n) {
    if (!active)
        return;
    ++calls;
    bytes += n;
    if (n <= 64)
        ++small;
    else if (n <= 1024)
        ++medium;
    else
        ++large;
}
void* allocate(std::size_t n) {
    auto p = std::malloc(n ? n : 1);
    if (!p)
        throw std::bad_alloc();
    record(n);
    return p;
}
} // namespace
void* operator new(std::size_t n) {
    return allocate(n);
}
void* operator new[](std::size_t n) {
    return allocate(n);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void* operator new(std::size_t n, std::align_val_t a) {
    void* p = nullptr;
    if (posix_memalign(&p, static_cast<std::size_t>(a), n ? n : 1))
        throw std::bad_alloc();
    record(n);
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return ::operator new(n, a);
}
void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
namespace allocation_profile {
void begin() {
    calls = bytes = small = medium = large = 0;
    start = std::clock();
    active = true;
}
void end() {
    active = false;
    std::fprintf(stderr,
                 "{\"allocation_profile\":true,\"new_calls\":%zu,\"requested_bytes\":%zu,\"up_to_64\":%zu,"
                 "\"up_to_1024\":%zu,\"above_1024\":%zu,\"cpu_seconds\":%.6f}\n",
                 calls, bytes, small, medium, large, double(std::clock() - start) / CLOCKS_PER_SEC);
}
} // namespace allocation_profile
