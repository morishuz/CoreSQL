#include "coresql/sql.hpp"
#include "coresql/date.hpp"
#include "coresql/decimal.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
void print(const coresql::Value& value) {
    if(coresql::is_null(value))std::cout<<"NULL";
    else if(auto n=std::get_if<std::int64_t>(&value))std::cout<<*n;
    else if(auto n=std::get_if<double>(&value))std::cout<<*n;
    else if(auto s=std::get_if<std::string>(&value)) {
        for(char c:*s)switch(c){case '\n':std::cout<<"\\n";break;case '\t':std::cout<<"\\t";break;case '\r':std::cout<<"\\r";break;case '\\':std::cout<<"\\\\";break;case '\0':std::cout<<"\\0";break;default:std::cout<<c;}
    } else if (coresql::type_of(value) == coresql::dates::type())
        std::cout << coresql::dates::format(value);
    else if (coresql::decimals::is_decimal(coresql::type_of(value)))
        std::cout << coresql::decimals::format(value);
    else
        throw std::runtime_error("CLI cannot display custom opaque values");
}
}
int main(int argc,char** argv) {
    try {
        std::string database,script;
        for(int i=1;i<argc;++i){std::string arg=argv[i];if(arg=="--database"&&i+1<argc)database=argv[++i];else if(script.empty())script=arg;else throw std::runtime_error("usage: coresql_sql_cli [--database path] [script.sql]; otherwise read stdin");}
        std::ifstream file;if(!script.empty()){file.open(script);if(!file)throw std::runtime_error("Cannot open SQL script");}
        auto& input=script.empty()?std::cin:file;
        std::string source{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        auto statements=coresql::sql::prepare_script(source);
        coresql::Registry registry;coresql::sql::install(registry);
        auto db=database.empty()?coresql::Database(registry):coresql::Database::open(database,registry);
        coresql::sql::Connection connection(db,registry);
        for(const auto& statement:statements)for(const auto& row:connection.execute(statement).rows){for(std::size_t i=0;i<row.size();++i){if(i)std::cout<<'\t';print(row[i]);}std::cout<<'\n';}
        if(connection.in_transaction())throw std::runtime_error("Script ended with an uncommitted transaction; changes rolled back");
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
