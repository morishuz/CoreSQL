#include "speedtest1_support.hpp"
#include "coresql/sql.hpp"
#include <cmath>
#include <sstream>

Value iv(unsigned n) { return static_cast<std::int64_t>(n); }
Predicate between(Expr e, Value lo, Value hi) { return all_of({{e,Compare::greater_equal,literal(lo)},{e,Compare::less_equal,literal(hi)}}); }
Expr mul(Expr e,unsigned n) { return call("integer.multiply",{e,literal(iv(n))}); }
Expr band(Expr e) { return call("integer.bitand",{e,literal(iv(1))}); }
Predicate like(unsigned n) { return {call("text.like_ascii",{column("c"),literal(pattern(n))}),Compare::equal,literal(iv(1))}; }
std::vector<Expr> reductions(std::string concat) { return {aggregate("count"),aggregate("avg",{column("b")}),aggregate("sum",{call("text.length",{column("c")})}),aggregate("group_concat",{column(concat)})}; }
struct Random {
    unsigned x=0xad131d0b,y=0x44f9eac8;
    unsigned next() { x=(x>>1)^((1+~(x&1))&0xd0000001); y=y*1103515245+12345; return x^y; }
};
Registry sql_registry() { Registry r;coresql::sql::install(r);return r; }
struct Backend {
    bool native,text_mode;
    Registry types=sql_registry();
    Database db{types};
    coresql::sql::Connection frontend{db,types};
    SQLite sqlite;
    std::optional<Transaction> tx;
    std::map<std::string,std::unique_ptr<Statement>> statements;
    std::map<std::string,coresql::sql::Statement> parsed;
    std::map<std::string,std::vector<Column>> schemas;
    std::vector<Rows> answers;
    double prepare_ms=0,execute_ms=0;
    explicit Backend(bool n,bool text=false):native(n),text_mode(n&&text) {}
    Rows sql_rows(const std::string& sql,std::vector<Value> args={}) {
        auto start=Clock::now();
        if(text_mode) {
            auto it=parsed.find(sql);
            if(it==parsed.end()){it=parsed.emplace(sql,coresql::sql::Statement(sql)).first;prepare_ms+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
            start=Clock::now();auto rows=frontend.execute(it->second,args).rows;
            execute_ms+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();return rows;
        }
        auto& statement=statements[sql];
        if(!statement){statement=std::make_unique<Statement>(sqlite,sql);prepare_ms+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
        start=Clock::now();for(std::size_t i=0;i<args.size();++i)statement->bind(static_cast<int>(i+1),args[i]);
        auto rows=statement->rows();execute_ms+=std::chrono::duration<double,std::milli>(Clock::now()-start).count();return rows;
    }
    void begin() { if(native&&!text_mode)tx.emplace(db.begin());else sql_rows("BEGIN"); }
    void commit() { if(native&&!text_mode){tx->commit();tx.reset();}else sql_rows("COMMIT"); }
    template<class F> void mutation(F f,const std::string& sql) {
        if(!native||text_mode){sql_rows(sql);return;}
        if(tx)f(*tx);else{auto t=db.begin();f(t);t.commit();}
    }
    Rows read(Query q,const std::string& sql,std::vector<Value> params={}) {
        if(native&&!text_mode)return tx?tx->query(q).rows:db.query(q).rows;
        return sql_rows(sql,std::move(params));
    }
    void query(Query q,const std::string& sql,std::vector<Value> p={}) {answers.push_back(read(std::move(q),sql,std::move(p)));}
    void create(const std::string& name,std::vector<Column> schema,const std::string& sql,bool unique=false) {
        schemas[name]=schema;
        mutation([&](auto& t){t.create_table(name,schema);if(unique)t.create_index(name,{name+"_a",{"a"},true});},sql);
    }
    void index(const std::string& table,IndexDefinition d,const std::string& sql){mutation([&](auto& t){t.create_index(table,d);},sql);}
    void insert(const std::string& table,Row row,bool replace=false) {
        if(native&&!text_mode){mutation([&](auto& t){if(replace)t.replace(table,row);else t.insert(table,row);},"");return;}
        std::string sql=std::string(replace?"REPLACE":"INSERT")+" INTO "+table+" VALUES(";
        for(std::size_t i=0;i<row.size();++i){if(i)sql+=',';sql+='?';}sql+=')';sql_rows(sql,std::move(row));
    }
    void from(const std::string& table,Query q,const std::string& sql,bool replace=false){mutation([&](auto& t){t.insert_from(table,q,replace);},sql);}
    void erase(const std::string& table,std::optional<Predicate> p,const std::string& sql,std::vector<Value> args={}) {
        if(native&&!text_mode)mutation([&](auto& t){t.erase(table,p);},"");else sql_rows(sql,std::move(args));
    }
    void update(Expr value,std::optional<Predicate> p,const std::string& sql,std::vector<Value> args={}) {
        if(native&&!text_mode)mutation([&](auto& t){t.update("z2",{{"d",value}},p);},"");else sql_rows(sql,std::move(args));
    }
};
void compare_rows(Rows a,Rows b,bool ordered,bool concat) {
    require(a.size()==b.size(),"Result row count differs");
    auto canonical=[](Value& v) {
        if(is_null(v))return;
        std::istringstream in(std::get<std::string>(v));std::vector<std::string> words;std::string word;
        while(std::getline(in,word,','))words.push_back(word);
        std::sort(words.begin(),words.end());std::string joined;
        for(const auto& w:words){joined+=w;joined+='\n';}v=joined;
    };
    if(concat) { for(auto& r:a)canonical(r[3]);for(auto& r:b)canonical(r[3]); }
    Registry registry;
    auto less=[&](const Row& x,const Row& y){for(std::size_t i=0;i<x.size();++i){if(is_null(x[i])||is_null(y[i])){if(is_null(x[i])!=is_null(y[i]))return is_null(x[i]);continue;}auto c=registry.compare(x[i],y[i]);if(c)return c<0;}return false;};
    if(!ordered){std::sort(a.begin(),a.end(),less);std::sort(b.begin(),b.end(),less);}
    for(std::size_t i=0;i<a.size();++i) {
        require(a[i].size()==b[i].size(),"Result width differs");
        for(std::size_t j=0;j<a[i].size();++j) {
            const auto& x=a[i][j];const auto& y=b[i][j];
            if(is_null(x)||is_null(y)) require(is_null(x)&&is_null(y),"NULL differs");
            else if(auto v=std::get_if<double>(&x)) require(std::holds_alternative<double>(y)&&std::abs(*v-std::get<double>(y))<=1e-10*std::max(1.,std::abs(*v)),"Real result differs");
            else require(registry.equal(x,y),"Result value differs");
        }
    }
}
int main(int argc,char** argv) {
 try {
    const unsigned size=argc>1?static_cast<unsigned>(std::stoul(argv[1])):1;
    const bool sql_mode=argc>2&&std::string(argv[2])=="sql";
    const unsigned repetitions=argc>3?static_cast<unsigned>(std::stoul(argv[3])):1;
    require(argc<=4&&size>=1&&size<=100&&repetitions>=1&&repetitions<=20&&(argc<3||sql_mode||std::string(argv[2])=="api"),"usage: coresql_speedtest1_main [size=1..100] [api|sql] [repetitions=1..20]");
    const unsigned n=size*500,mask=roundup_allones(n);
    if(sql_mode)std::cout<<"run,test,first,coresql_ms,sqlite_ms,coresql_prepare_ms,sqlite_prepare_ms,coresql_execute_ms,sqlite_execute_ms\n";
    else std::cout<<"test,coresql_ms,sqlite_ms\n";
    for(unsigned run=0;run<repetitions;++run) {
    Backend core(true,sql_mode),sql(false);
    std::cerr<<"Full adapted main; size="<<size<<"; SQLite "<<sqlite3_libversion()<<" "<<sqlite3_sourceid()<<"\n";
    auto measure=[](auto f){auto start=Clock::now();f();return std::chrono::duration<double,std::milli>(Clock::now()-start).count();};
    auto test=[&](int id,auto body) {
        core.answers.clear();sql.answers.clear();
        core.prepare_ms=core.execute_ms=sql.prepare_ms=sql.execute_ms=0;
        double a,b;
        if(run%2){b=measure([&]{body(sql);});a=measure([&]{body(core);});}
        else{a=measure([&]{body(core);});b=measure([&]{body(sql);});}
        const auto cp=core.prepare_ms,ce=core.execute_ms,sp=sql.prepare_ms,se=sql.execute_ms;
        try {
            require(core.answers.size()==sql.answers.size(),"Query count differs");
            for(std::size_t i=0;i<core.answers.size();++i)compare_rows(core.answers[i],sql.answers[i],id==142||id==145,id==130||id==140||id==160||id==161||id==170);
            for(const auto& [name,schema]:core.schemas) {
                (void)schema; compare_rows(core.read(Query{name,{}, {},{Order{column("a")}}},"SELECT * FROM "+name+" ORDER BY a"),sql.read({},"SELECT * FROM "+name+" ORDER BY a"),true,false);
            }
        }catch(const std::exception& e){throw std::runtime_error("case "+std::to_string(id)+": "+e.what());}
        // Fresh prepared statements per case, as in upstream. Verification is untimed.
        core.statements.clear();sql.statements.clear();core.parsed.clear();
        if(sql_mode)std::cout<<run+1<<','<<id<<','<<(run%2?"sqlite":"coresql")<<','<<a<<','<<b<<','<<cp<<','<<sp<<','<<ce<<','<<se<<std::endl;
        else std::cout<<id<<','<<a<<','<<b<<std::endl;
    };
    for(int t=0;t<3;++t)test(100+t*10,[&](Backend& b){
        std::string name=t==0?"z1":t==1?"z2":"t3";
        b.begin(); b.create(name,{{"a",integer()},{"b",integer()},{"c",text()}},"CREATE TABLE "+name+"(a INTEGER"+(t?" UNIQUE":"")+",b INTEGER,c TEXT)",t!=0);
        for(unsigned i=1;i<=n;++i)b.insert(name,input(i,mask,t==1));b.commit();
    });
    test(130,[&](Backend& b){Random rng;b.begin();for(unsigned i=0;i<25;++i){auto lo=rng.next()%mask,hi=rng.next()%10+n/5000+lo;b.query({"z1",reductions("c"),between(column("b"),iv(lo),iv(hi))},"SELECT count(*),avg(b),sum(length(c)),group_concat(c) FROM z1 WHERE b BETWEEN ? AND ?",{iv(lo),iv(hi)});}b.commit();});
    test(140,[&](Backend& b){b.begin();for(unsigned i=1;i<=10;++i)b.query({"z1",reductions("c"),like(i)},"SELECT count(*),avg(b),sum(length(c)),group_concat(c) FROM z1 WHERE c LIKE ?",{pattern(i)});b.commit();});
    for(int id:{142,145})test(id,[&](Backend& b){b.begin();for(unsigned i=1;i<=10;++i)b.query({"z1",{},like(i),{Order{column("a")}},id==145?10:std::numeric_limits<std::size_t>::max()},std::string("SELECT a,b,c FROM z1 WHERE c LIKE ? ORDER BY a")+(id==145?" LIMIT 10":""),{pattern(i)});b.commit();});
    test(150,[&](Backend& b){b.begin();b.index("z1",{"t1b",{"b"},true},"CREATE UNIQUE INDEX t1b ON z1(b)");b.index("z1",{"t1c",{"c"}},"CREATE INDEX t1c ON z1(c)");b.index("z2",{"t2b",{"b"},true},"CREATE UNIQUE INDEX t2b ON z2(b)");b.index("z2",{"t2c",{"c"},false,{true}},"CREATE INDEX t2c ON z2(c DESC)");b.index("t3",{"t3bc",{"b","c"}},"CREATE INDEX t3bc ON t3(b,c)");b.commit();});
    for(int id:{160,161})test(id,[&](Backend& b){Random rng;b.begin();const std::string table=id==160?"z1":"z2",col=id==160?"b":"a";for(unsigned i=0;i<n/5;++i){auto lo=rng.next()%mask,hi=rng.next()%10+n/5000+lo;b.query({table,reductions("a"),between(column(col),iv(lo),iv(hi))},"SELECT count(*),avg(b),sum(length(c)),group_concat(a) FROM "+table+" WHERE "+col+" BETWEEN ? AND ?",{iv(lo),iv(hi)});}b.commit();});
    test(170,[&](Backend& b){b.begin();for(unsigned i=1;i<=n/5;++i){auto key=number(swizzle(i,mask));b.query({"z1",reductions("a"),between(column("c"),key,key+"~")},"SELECT count(*),avg(b),sum(length(c)),group_concat(a) FROM z1 WHERE c BETWEEN ? AND (?||'~')",{key,key});}b.commit();});
    test(180,[&](Backend& b){b.begin();b.create("t4",{{"a",integer()},{"b",integer()},{"c",text()}},"CREATE TABLE t4(a INTEGER UNIQUE,b INTEGER,c TEXT)",true);b.index("t4",{"t4b",{"b"}},"CREATE INDEX t4b ON t4(b)");b.index("t4",{"t4c",{"c"}},"CREATE INDEX t4c ON t4(c)");b.from("t4",{"z1",{}},"INSERT INTO t4 SELECT * FROM z1");b.commit();});
    test(190,[&](Backend& b){b.erase("z2",{},"DELETE FROM z2");b.from("z2",{"z1",{}},"INSERT INTO z2 SELECT * FROM z1");});
    test(200,[&](Backend& b){b.mutation([](auto& t){t.vacuum();},"VACUUM");});
    test(210,[&](Backend& b){Column c{"d",integer(),false,{},iv(123)};b.mutation([&](auto& t){t.add_column("z2",c);},"ALTER TABLE z2 ADD COLUMN d INT DEFAULT 123");b.schemas["z2"].push_back(c);b.query({"z2",{aggregate("sum",{column("d")})}},"SELECT sum(d) FROM z2");});
    test(230,[&](Backend& b){Random rng;b.begin();for(unsigned i=0;i<n/5;++i){auto lo=rng.next()%mask,hi=rng.next()%10+n/5000+lo;b.update(mul(column("b"),2),between(column("b"),iv(lo),iv(hi)),"UPDATE z2 SET d=b*2 WHERE b BETWEEN ? AND ?",{iv(lo),iv(hi)});}b.commit();});
    test(240,[&](Backend& b){Random rng;b.begin();for(unsigned i=0;i<n;++i){auto key=rng.next()%n+1;b.update(mul(column("b"),3),Predicate{column("a"),Compare::equal,literal(iv(key))},"UPDATE z2 SET d=b*3 WHERE a=?",{iv(key)});}b.commit();});
    test(250,[&](Backend& b){b.update(mul(column("b"),4),{},"UPDATE z2 SET d=b*4");});
    test(260,[&](Backend& b){b.query({"z2",{aggregate("sum",{column("d")})}},"SELECT sum(d) FROM z2");});
    test(270,[&](Backend& b){Random rng;b.begin();for(unsigned i=0;i<n/5;++i){auto lo=rng.next()%mask+1,hi=rng.next()%10+n/5000+lo;b.erase("z2",between(column("b"),iv(lo),iv(hi)),"DELETE FROM z2 WHERE b BETWEEN ? AND ?",{iv(lo),iv(hi)});}b.commit();});
    test(280,[&](Backend& b){Random rng;b.begin();for(unsigned i=0;i<n;++i){auto key=rng.next()%n+1;b.erase("t3",Predicate{column("a"),Compare::equal,literal(iv(key))},"DELETE FROM t3 WHERE a=?",{iv(key)});}b.commit();});
    test(290,[&](Backend& b){for(std::string table:{"z2","t3"})b.from(table,{"z1",{column("a"),column("b"),column("c")}},"REPLACE INTO "+table+"(a,b,c) SELECT a,b,c FROM z1",true);});
    test(300,[&](Backend& b){b.erase("z2",{},"DELETE FROM z2");for(bool equal:{true,false})b.from("z2",{"z1",{column("a"),column("b"),column("c")},Predicate{band(column("b")),equal?Compare::equal:Compare::not_equal,band(column("a"))}},std::string("INSERT INTO z2(a,b,c) SELECT a,b,c FROM z1 WHERE (b&1)")+(equal?"==":"<>")+"(a&1)");});
    test(310,[&](Backend& b){Random rng;b.begin();for(unsigned i=0;i<n/5;++i){auto lo=rng.next()%n+1,hi=rng.next()%10+lo+4;Query q{"t4",{column("z1","c")}};q.alias="t4";q.source_where=between(column("a"),iv(lo),iv(hi));q.joins={{"t3","t3",column("t4","b"),column("t3","a")},{"z2","z2",column("t3","b"),column("z2","a")},{"z1","z1",column("z2","c"),column("z1","c")}};b.query(q,"SELECT z1.c FROM z1,z2,t3,t4 WHERE t4.a BETWEEN ? AND ? AND t3.a=t4.b AND z2.a=t3.b AND z1.c=z2.c",{iv(lo),iv(hi)});}b.commit();});
    test(320,[&](Backend& b){unsigned root=size/2;for(unsigned j=0;root>0&&j<10;++j){auto next=(root+size/root)/2;if(next==root)break;root=next;}unsigned limit=root*50;Query sub{"z2",{column("a")},Predicate{call("integer.add",{literal(iv(5)),column("b")}),Compare::equal,parameter("outer_b")}};auto value=scalar_subquery(sub,{{"outer_b",column("b")}});b.query({"z1",{aggregate("sum",{column("a")}),aggregate("max",{column("c")}),aggregate("avg",{call("integer.and",{value,literal(iv(1))})}),aggregate("max",{column("c")})},Predicate{row_id(),Compare::less,literal(iv(limit))}},"SELECT sum(a),max(c),avg((SELECT a FROM z2 WHERE 5+z2.b=z1.b) AND rowid<?),max(c) FROM z1 WHERE rowid<?",{iv(limit),iv(limit)});});
    const unsigned replaces=size*700,replace_mask=roundup_allones(replaces/3);
    for(int id:{400,500}){
        const std::string table=id==400?"t5":"t6";
        test(id,[&](Backend& b){b.begin();b.create(table,{{"a",id==400?integer():text()},{"b",id==400?text():integer()}},id==400?"CREATE TABLE t5(a INTEGER PRIMARY KEY,b)":"CREATE TABLE t6(a TEXT PRIMARY KEY,b) WITHOUT ROWID",true);for(unsigned i=1;i<=replaces;++i){auto key=swizzle(i,replace_mask);if(id==400)b.insert(table,{iv(key),number(i)},true);else b.insert(table,{number(key),iv(i)},true);}b.commit();});
        test(id+10,[&](Backend& b){for(unsigned i=1;i<=replaces;++i){auto key=swizzle(i,replace_mask);Value value=id==400?iv(key):Value(number(key));b.query({table,{column("b")},Predicate{column("a"),Compare::equal,literal(value)}},"SELECT b FROM "+table+" WHERE a=?",{value});}});
    }
    test(520,[&](Backend& b){for(std::string table:{"t5","t6"}){Query q{table,{column("b")}};q.distinct=true;b.query(q,"SELECT DISTINCT b FROM "+table);}});
    test(980,[&](Backend& b){if(b.native&&!b.text_mode){auto t=b.db.begin();t.integrity_check();}else {auto result=b.read({},"PRAGMA integrity_check");require(result.size()==1&&std::get<std::string>(result[0][0])=="ok","SQLite integrity failure");}});
    test(990,[&](Backend& b){b.mutation([](auto& t){require(!t.analyze().empty(),"Missing statistics");},"ANALYZE");});
    std::cerr<<"All 32 adapted cases passed query and post-case table comparisons. Maintenance uses native equivalents.\n";
    }
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
