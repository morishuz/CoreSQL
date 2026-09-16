#include "ast.hpp"
#include <limits>

namespace coresql::sql::detail {
void install_string_functions(Registry& r) {
    r.add(Function{"text.substring",
                   [](std::span<const Type> t) {
                       if ((t.size() != 2 && t.size() != 3) || t[0] != text() || t[1] != integer() ||
                           (t.size() == 3 && t[2] != integer()))
                           throw Error(ErrorCode::type,
                                       "SUBSTRING expects TEXT, INTEGER and optional INTEGER");
                       return text();
                   },
                   [](std::span<const Value> v) -> Value {
                       const auto& s = std::get<std::string>(v[0]);
                       auto start = std::get<std::int64_t>(v[1]);
                       auto count = v.size() == 3 ? std::get<std::int64_t>(v[2]) : INT64_MAX;
                       if (count < 0)
                           throw Error(ErrorCode::constraint, "Negative SUBSTRING length");
                       std::vector<std::size_t> offsets;
                       for (std::size_t i = 0; i < s.size();) {
                           offsets.push_back(i);
                           auto c = static_cast<unsigned char>(s[i++]);
                           unsigned rest = c < 0x80                 ? 0
                                           : c >= 0xc2 && c <= 0xdf ? 1
                                           : c >= 0xe0 && c <= 0xef ? 2
                                           : c >= 0xf0 && c <= 0xf4 ? 3
                                                                    : 4;
                           if (rest == 4 || rest > s.size() - i)
                               throw Error(ErrorCode::constraint, "SUBSTRING requires valid UTF-8");
                           if (rest && ((c == 0xe0 && static_cast<unsigned char>(s[i]) < 0xa0) ||
                                        (c == 0xed && static_cast<unsigned char>(s[i]) >= 0xa0) ||
                                        (c == 0xf0 && static_cast<unsigned char>(s[i]) < 0x90) ||
                                        (c == 0xf4 && static_cast<unsigned char>(s[i]) >= 0x90)))
                               throw Error(ErrorCode::constraint, "SUBSTRING requires valid UTF-8");
                           while (rest--) {
                               auto next = static_cast<unsigned char>(s[i++]);
                               if (next < 0x80 || next > 0xbf)
                                   throw Error(ErrorCode::constraint, "SUBSTRING requires valid UTF-8");
                           }
                       }
                       offsets.push_back(s.size());
                       auto index = [&](std::int64_t position) {
                           return position <= 1
                                      ? std::size_t{0}
                                      : std::min(static_cast<std::size_t>(position - 1), offsets.size() - 1);
                       };
                       auto end = v.size() == 2 || start > INT64_MAX - count ? INT64_MAX : start + count;
                       auto a = offsets[index(start)], b = offsets[index(end)];
                       return s.substr(a, b - a);
                   }});
}
} // namespace coresql::sql::detail
