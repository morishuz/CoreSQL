function(coresql_check_toolchain)
  # A C++20 language flag alone does not guarantee floating charconv support.
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles([[
    #include <charconv>
    #include <system_error>
    int main() {
      char buffer[64];
      auto out = std::to_chars(buffer, buffer + sizeof(buffer), 1.25);
      double value = 0;
      auto in = std::from_chars(buffer, out.ptr, value);
      return out.ec == std::errc{} && in.ec == std::errc{} ? 0 : 1;
    }
  ]] CORESQL_HAS_FLOAT_CHARCONV)
  if(NOT CORESQL_HAS_FLOAT_CHARCONV)
    message(FATAL_ERROR "CoreSQL needs a C++20 standard library with floating-point std::from_chars and std::to_chars. Select a newer compiler/SDK; see CONTRIBUTING.md.")
  endif()
endfunction()
coresql_check_toolchain()
