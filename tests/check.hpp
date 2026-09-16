#pragma once
#include "coresql/core.hpp"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)
template<class F> void expect(coresql::ErrorCode code, F&& fn) {
    try { fn(); }
    catch (const coresql::Error& error) { CHECK(error.code == code); return; }
    throw std::runtime_error("Expected CoreSQL error");
}
struct TempDirectory {
    std::filesystem::path path;
    TempDirectory() {
        auto pattern = (std::filesystem::temp_directory_path() / "coresql-test-XXXXXX").string();
        auto* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create test directory");
        path = created;
    }
    ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
template<class F> int tests(F&& fn) {
    try { fn(); std::cout << "All checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
