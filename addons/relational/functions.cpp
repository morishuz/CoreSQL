#include "coresql/core.hpp"

namespace coresql {
void install_relational_functions(Registry& r) {
    auto binary_integer=[](std::span<const Type> t) {
        if(t.size()!=2 || t[0]!=integer() || t[1]!=integer()) throw Error(ErrorCode::type,"Expected two integers"); return integer();
    };
    for (std::string name : {"add","multiply","bitand"}) r.add(Function{"integer."+name,binary_integer,[name](std::span<const Value> v)->Value {
        auto a=std::get<std::int64_t>(v[0]), b=std::get<std::int64_t>(v[1]), result=std::int64_t{};
        if(name=="bitand") return a&b;
        bool overflow=name=="add" ? __builtin_add_overflow(a,b,&result) : __builtin_mul_overflow(a,b,&result);
        if(overflow) throw Error(ErrorCode::constraint,"Integer arithmetic overflow"); return result;
    }});
    r.add(Function{"text.length",[](std::span<const Type> t) { if(t.size()!=1||t[0]!=text()) throw Error(ErrorCode::type,"Length needs text"); return integer(); },
        [](std::span<const Value> v)->Value {
            std::int64_t n=0; for(char byte:std::get<std::string>(v[0])) { auto c=static_cast<unsigned char>(byte); if(!c) break; if((c&0xc0)!=0x80) ++n; } return n;
        }});
    r.add(Function{"text.like_ascii",[](std::span<const Type> t) { if(t.size()!=2||t[0]!=text()||t[1]!=text()) throw Error(ErrorCode::type,"LIKE needs text"); return integer(); },
        [](std::span<const Value> v)->Value {
            const auto& s=std::get<std::string>(v[0]); const auto& p=std::get<std::string>(v[1]);
            auto fold=[](unsigned char c) { return c>='A' && c<='Z' ? c+('a'-'A') : c; };
            std::size_t i=0,j=0,star=std::string::npos,mark=0;
            while(i<s.size()) {
                if(j<p.size() && (p[j]=='_' || (p[j]!='%' && fold(static_cast<unsigned char>(s[i]))==fold(static_cast<unsigned char>(p[j]))))) { ++i; ++j; }
                else if(j<p.size() && p[j]=='%') { star=j++; mark=i; }
                else if(star!=std::string::npos) { j=star+1; i=++mark; }
                else return std::int64_t{0};
            }
            while(j<p.size() && p[j]=='%') ++j;
            return std::int64_t{j==p.size()};
        }});
    r.add(Function{"integer.and",binary_integer,[](std::span<const Value> v)->Value {
        if ((!is_null(v[0]) && !std::get<std::int64_t>(v[0])) || (!is_null(v[1]) && !std::get<std::int64_t>(v[1]))) return std::int64_t{0};
        if(is_null(v[0])||is_null(v[1])) return Null(integer());
        return std::int64_t{1};
    },true});
}
}
