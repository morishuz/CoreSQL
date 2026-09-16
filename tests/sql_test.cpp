#include "check.hpp"
#include "coresql/sql.hpp"
#include "coresql/graph.hpp"
#include "../src/state.hpp"
using namespace coresql;
int main(){return tests([]{
    int ticks=0;
    Registry r;graph::install(r);
    r.add(Function{"tick",[](std::span<const Type> t){if(!t.empty())throw Error(ErrorCode::type,"No arguments expected");return integer();},[&](std::span<const Value>)->Value{++ticks;return std::int64_t{1};}});
    r.add(Function{"fail",[](std::span<const Type> t){if(!t.empty())throw Error(ErrorCode::type,"No arguments expected");return integer();},[](std::span<const Value>)->Value{throw Error(ErrorCode::constraint,"Injected callback failure");}});
    sql::install(r);Database db(r);sql::Connection c(db,r);
    CHECK(c.execute("SELECT abs(-7),abs(7),abs(0),abs(-1.25),abs(NULL)").rows[0]==Row({std::int64_t{7},std::int64_t{7},std::int64_t{0},1.25,Null(integer())}));
    expect(ErrorCode::constraint,[&]{c.execute("SELECT abs(-9223372036854775808)");});
    CHECK(c.execute("SELECT abs('text')").rows[0][0]==Value(0.0));
    expect(ErrorCode::type,[&]{c.execute("SELECT abs(1,2)");});
    ticks=0;
    CHECK(c.execute("SELECT tick() IN (0,1,NULL)").rows[0][0]==Value(std::int64_t{1}));CHECK(ticks==1);
    CHECK(c.execute("SELECT NULL NOT IN ()").rows[0][0]==Value(std::int64_t{1}));
    CHECK(c.execute("SELECT 'x' IN (NULL,'x'),9007199254740993 IN (9007199254740992.0)").rows[0]==Row({std::int64_t{1},std::int64_t{0}}));
    expect(ErrorCode::type,[&]{c.execute("SELECT 1 IN (missing()) LIMIT 0");});
    ticks=0;
    std::string wide="SELECT 1";for(int i=0;i<100;++i)wide+=" AND 1";CHECK(c.execute(wide).rows[0][0]==Value(std::int64_t{1}));
    CHECK(c.execute("SELECT coalesce(NULL,5,fail())").rows[0][0]==Value(std::int64_t{5}));
    CHECK(c.execute("SELECT coalesce(tick(),fail())").rows[0][0]==Value(std::int64_t{1}));CHECK(ticks==1);ticks=0;
    CHECK(c.execute("SELECT coalesce(NULL,'text')").rows[0][0]==Value(std::string("text")));
    CHECK(c.execute("SELECT coalesce(1,'text')").rows[0][0]==Value(std::int64_t{1}));
    CHECK(c.execute("SELECT tick() NOT BETWEEN 0 AND 2").rows[0][0]==Value(std::int64_t{0}));CHECK(ticks==1);ticks=0;
    CHECK(c.execute("SELECT CASE NULL WHEN 'text' THEN 1 ELSE 2 END").rows[0][0]==Value(std::int64_t{2}));
    CHECK(c.execute("SELECT CASE 'text' WHEN NULL THEN 1 ELSE 2 END").rows[0][0]==Value(std::int64_t{2}));
    CHECK(c.execute("SELECT CASE NULL WHEN fail() THEN 7 ELSE 8 END").rows[0][0]==Value(std::int64_t{8}));
    expect(ErrorCode::constraint,[&]{c.execute("SELECT 1 NOT BETWEEN fail() AND 2");});
    CHECK(c.execute("SELECT CASE 9007199254740993 WHEN 9007199254740992.0 THEN 1 ELSE 2 END").rows[0][0]==Value(std::int64_t{2}));
    CHECK(c.execute("SELECT CASE WHEN 1 THEN 7 ELSE fail() END").rows[0][0]==Value(std::int64_t{7}));
    CHECK(c.execute("SELECT CASE tick() WHEN 0 THEN fail() WHEN 1 THEN 8 WHEN fail() THEN 9 END").rows[0][0]==Value(std::int64_t{8}));CHECK(ticks==1);
    CHECK(c.execute("SELECT CASE WHEN NULL THEN fail() ELSE 8 END").rows[0][0]==Value(std::int64_t{8}));
    CHECK(is_null(c.execute("SELECT CASE WHEN 0 THEN 'text' END").rows[0][0]));
    CHECK(c.execute("SELECT CASE WHEN 0 THEN fail() WHEN 1 THEN CASE 2 WHEN 2 THEN 3 END END").rows[0][0]==Value(std::int64_t{3}));
    expect(ErrorCode::constraint,[&]{c.execute("SELECT CASE WHEN 0 THEN 7 ELSE fail() END");});
    CHECK(c.execute("SELECT CASE WHEN 1 THEN 7 ELSE 'text' END").rows[0][0]==Value(std::int64_t{7}));
    expect(ErrorCode::type,[&]{c.execute("SELECT CASE WHEN 1 THEN 7 ELSE missing() END");});
    ticks = 0;
    CHECK(c.execute("SELECT tick() NOT LIKE '2%'").rows[0][0] == Value(std::int64_t{1}));
    CHECK(ticks == 1);
    expect(ErrorCode::constraint, [&] { c.execute("SELECT 1 NOT LIKE fail()"); });
    expect(ErrorCode::type, [&] { c.execute("SELECT 1 NOT LIKE missing() LIMIT 0"); });
    CHECK(c.execute("SELECT CASE WHEN 1 THEN 7 ELSE 1 NOT LIKE fail() END").rows[0][0] ==
          Value(std::int64_t{7}));
    c.execute("CREATE TABLE items(id INTEGER PRIMARY KEY, label TEXT)");
    sql::Statement insert("INSERT INTO items VALUES(?1,?2); -- reusable");
    CHECK(insert.parameter_count()==2);
    c.execute("BEGIN");for(std::int64_t i=1;i<=3;++i)c.execute(insert,Row{i,std::string("label")+std::to_string(i)});
    expect(ErrorCode::constraint,[&]{c.execute(insert,Row{std::int64_t{1},std::string("duplicate")});});
    CHECK(c.in_transaction());c.execute("COMMIT");
    Query guarded{"items",{column("a","missing")}};guarded.alias="a";
    guarded.join=InnerJoin{"items","b",literal(std::int64_t{0}),literal(std::int64_t{0}),true};
    guarded.join->right_where=Predicate{call("fail",{}),Compare::equal,literal(std::int64_t{1})};
    expect(ErrorCode::schema,[&]{db.query(guarded);});
    guarded.select={column("a","id")};guarded.limit=0;CHECK(db.query(guarded).rows.empty());
    ticks=0;CHECK(c.execute("SELECT id FROM items WHERE tick() NOT BETWEEN 0 AND 2").rows.empty());CHECK(ticks==3);
    CHECK(c.execute("SELECT CASE WHEN id=1 THEN label ELSE NULL END FROM items ORDER BY id,label").rows[0][0]==Value(std::string("label1")));
    CHECK(c.execute("SELECT CASE WHEN 1 THEN id ELSE 'text' END FROM items LIMIT 0").rows.empty());
    c.execute("BEGIN");c.execute("UPDATE items SET id=CASE WHEN id=1 THEN 11 ELSE id END");c.execute("ROLLBACK");
    CHECK(c.execute("SELECT EXISTS(SELECT fail() FROM items),NOT EXISTS(SELECT NULL FROM items WHERE id<0) FROM items LIMIT 1").rows[0]==Row({std::int64_t{1},std::int64_t{1}}));
    CHECK(c.execute("SELECT EXISTS(SELECT id FROM items ORDER BY fail()),EXISTS(SELECT fail() FROM items LIMIT 0) FROM items LIMIT 1").rows[0]==Row({std::int64_t{1},std::int64_t{0}}));
    CHECK(c.execute("SELECT EXISTS(SELECT count(*) FROM items WHERE id<0),EXISTS(SELECT count(*) FROM items LIMIT 0) FROM items LIMIT 1").rows[0]==Row({std::int64_t{1},std::int64_t{0}}));
    expect(ErrorCode::constraint,[&]{c.execute("SELECT EXISTS(SELECT id FROM items WHERE fail()=1) FROM items LIMIT 1");});
    expect(ErrorCode::schema,[&]{c.execute("SELECT EXISTS(SELECT missing FROM items) FROM items LIMIT 0");});
    CHECK(c.execute("SELECT CASE WHEN 1 THEN 7 ELSE EXISTS(SELECT id FROM items WHERE fail()=1) END FROM items LIMIT 1").rows[0][0]==Value(std::int64_t{7}));
    auto rows=c.execute("SELECT id,label FROM items WHERE id BETWEEN ?1 AND ?2 ORDER BY id DESC LIMIT 2",Row{std::int64_t{1},std::int64_t{3}}).rows;
    CHECK(rows.size()==2&&std::get<std::int64_t>(rows[0][0])==3);
    CHECK(std::get<std::int64_t>(c.execute("SELECT sum(id),count(*) FROM items").rows[0][0])==6);
    c.execute("UPDATE items SET id=id+10 WHERE id=1");c.execute("BEGIN");c.execute("DELETE FROM items");c.execute("ROLLBACK");CHECK(c.execute("SELECT * FROM items").rows.size()==3);
    expect(ErrorCode::type,[&]{c.execute(insert,Row{std::int64_t{4}});});
    CHECK(c.execute("SELECT id FROM items GROUP BY id").rows.size()==3);
    expect(ErrorCode::unsupported,[&]{c.execute("DELETE FROM items; DELETE FROM items");});
    expect(ErrorCode::unsupported,[&]{sql::Statement bad("SELECT 'unterminated");});
    CHECK(c.execute("SELECT 'it''s; a string', 2+3*4, NULL AND 0").rows[0][1]==Value(std::int64_t{14}));
    CHECK(c.execute("SELECT 'it''s; a string', 2+3*4, NULL AND 0").rows[0][2]==Value(std::int64_t{0}));
    CHECK(c.execute("SELECT id FROM items WHERE 0 AND fail()=1").rows.empty());
    CHECK(c.execute("SELECT items.id FROM items JOIN items AS other ON fail()=1 LIMIT 0").rows.empty());
    expect(ErrorCode::constraint,[&]{c.execute("SELECT id FROM items WHERE fail()=1 AND 0");});
    expect(ErrorCode::constraint,[&]{c.execute("SELECT items.id FROM items,items AS other WHERE items.id=other.id AND fail()=1");});
    c.execute("CREATE TABLE dynamic(id INTEGER UNIQUE, value)");c.execute("INSERT INTO dynamic VALUES(1,42),(2,'text'),(3,2.5)");
    CHECK(c.execute("SELECT value FROM dynamic ORDER BY id").rows[0][0]==Value(std::int64_t{42}));
    c.execute("CREATE INDEX item_labels ON items(label)");
    detail::visited_chunks()=0;
    CHECK(c.execute("SELECT id FROM items WHERE label BETWEEN ? AND (?||'~')",Row{std::string("zzz"),std::string("zzz")}).rows.empty());
    CHECK(detail::visited_chunks()==0); // Bound concatenation must retain the index path.
    c.execute("CREATE TABLE copy(id INTEGER UNIQUE, label TEXT, extra INT DEFAULT 9)");
    c.execute("INSERT INTO copy(id,label) SELECT id,label FROM items");
    expect(ErrorCode::constraint,[&]{c.execute("INSERT INTO copy(id,label) VALUES(100,'new'),(11,'duplicate')");});
    CHECK(c.execute("SELECT * FROM copy WHERE id=100").rows.empty());
    expect(ErrorCode::schema,[&]{c.execute("INSERT INTO copy(id,label) SELECT id FROM items WHERE id<0");});
    CHECK(c.execute("INSERT INTO copy(id,label) SELECT label,id FROM items WHERE id<0").changes==0);
    expect(ErrorCode::type,[&]{c.execute("INSERT INTO copy(id,label) SELECT label,id FROM items");});
    c.execute("CREATE INDEX labels ON copy(label DESC)");c.execute("ALTER TABLE copy ADD COLUMN next INT DEFAULT 5");
    auto raw=db.begin();raw.create_table("edges",{{"e",graph::edge_type()}});raw.commit();c.execute("INSERT INTO edges VALUES(?)",Row{graph::value(graph::Edge{1,2,"x",1})});
    CHECK(c.execute("SELECT \"graph.edge.source\"(e) FROM edges").rows[0][0]==Value(std::int64_t{1}));
    CHECK(c.execute("SELECT id,(SELECT id FROM copy WHERE copy.id=items.id) FROM items ORDER BY id").rows.size()==3);
    // Typed outer captures must preserve indexed inner predicates.
    // Compare visited chunks rather than elapsed time.
    c.execute("CREATE TABLE capture_keys(id INTEGER)");
    c.execute("INSERT INTO capture_keys VALUES(10000),(20000)");
    detail::visited_chunks()=0;
    auto outer_rows=c.execute("SELECT id FROM capture_keys").rows;
    auto outer_chunks=detail::visited_chunks();
    detail::visited_chunks()=0;
    auto misses=c.execute("SELECT id,(SELECT id FROM copy WHERE copy.id=capture_keys.id) FROM capture_keys").rows;
    CHECK(misses.size()==outer_rows.size());
    for(const auto& row:misses)CHECK(is_null(row[1]));
    CHECK(detail::visited_chunks()==outer_chunks);
    detail::visited_chunks() = 0;
    auto joined_misses = c.execute("SELECT id,(SELECT x.id FROM copy x,items y WHERE "
                                   "x.id=capture_keys.id AND x.id=y.id) FROM capture_keys")
                             .rows;
    CHECK(joined_misses == misses);
    CHECK(detail::visited_chunks() == outer_chunks);
    CHECK(c.execute("SELECT id,(SELECT min(x.id) FROM copy x,items y WHERE x.id=capture_keys.id AND "
                    "x.id=y.id AND fail()=1) FROM capture_keys")
              .rows == misses);
    expect(ErrorCode::constraint, [&] {
        c.execute("SELECT id,(SELECT min(x.id) FROM copy x,items y WHERE fail()=1 AND x.id=capture_keys.id "
                  "AND x.id=y.id) FROM capture_keys");
    });

    CHECK(c.execute("SELECT (SELECT (SELECT capture_keys.id)) FROM capture_keys").rows==outer_rows);
    CHECK(c.execute("SELECT items.label FROM items,copy WHERE copy.id BETWEEN 1 AND 20 AND items.id=copy.id").rows.size()==3);
    detail::visited_chunks()=0;
    CHECK(c.execute("SELECT items.id FROM items,copy WHERE copy.id BETWEEN 10000 AND 20000 AND items.id=copy.id").rows.empty());
    CHECK(detail::visited_chunks()==0); // Flattened source predicates preserve range access.
    CHECK(c.execute("PRAGMA integrity_check").rows[0][0]==Value(std::string("ok")));
    CHECK(c.execute("SELECT -1.25,-9223372036854775808").rows[0][0]==Value(-1.25));
    CHECK(c.execute("SELECT -1.25,-9223372036854775808").rows[0][1]==Value(std::int64_t{INT64_MIN}));
    expect(ErrorCode::unsupported,[&]{c.execute("CREATE TABLE no_rowid(id INTEGER PRIMARY KEY) WITHOUT ROWID");});
    CHECK(sql::prepare_script("-- comment\n SELECT ';'; /* ; */ SELECT 2;").size()==2);
    CHECK(sql::prepare_script("; -- nothing\n ;").empty());
    expect(ErrorCode::unsupported,[&]{sql::Statement bad("SELECT ? 1");});
    expect(ErrorCode::unsupported,[&]{sql::Statement bad("SELECT ?4097");});
    std::string deep="SELECT 1";for(int i=0;i<1000;++i)deep+="+1";
    expect(ErrorCode::unsupported,[&]{sql::Statement bad(deep);});
    expect(ErrorCode::unsupported,[&]{sql::Statement bad("SELECT "+std::string(1000,'(')+"1"+std::string(1000,')'));});
    TempDirectory temp;db.save(temp.path/"snapshot");auto loaded=Database::load(temp.path/"snapshot",r);sql::Connection reopened(loaded,r);CHECK(reopened.execute("SELECT value FROM dynamic ORDER BY id").rows==c.execute("SELECT value FROM dynamic ORDER BY id").rows);
    {auto durable=Database::open(temp.path/"durable",r);sql::Connection connection(durable,r);
     connection.execute("CREATE TABLE values_table(id INTEGER,value)");connection.execute("INSERT INTO values_table VALUES(1,'persisted')");}
    {auto durable=Database::open(temp.path/"durable",r);sql::Connection connection(durable,r);
     CHECK(connection.execute("SELECT value FROM values_table").rows[0][0]==Value(std::string("persisted")));}

    {
        std::vector<std::int64_t> visited;
        Registry trace_registry;
        trace_registry.add(Function{"visit",
                                    [](std::span<const Type> types) {
                                        CHECK(types.size() == 1 && types[0] == integer());
                                        return integer();
                                    },
                                    [&](std::span<const Value> values) -> Value {
                                        visited.push_back(std::get<std::int64_t>(values[0]));
                                        return std::int64_t{1};
                                    }});
        sql::install(trace_registry);
        Database trace_db(trace_registry);
        sql::Connection trace(trace_db, trace_registry);
        trace.execute("CREATE TABLE p(id INTEGER)");
        trace.execute("CREATE TABLE q(id INTEGER)");
        trace.execute("CREATE TABLE z(id INTEGER)");
        trace.execute("INSERT INTO p VALUES(1),(2)");
        trace.execute("INSERT INTO q VALUES(1),(2)");
        trace.execute("INSERT INTO z VALUES(1),(2),(3)");
        CHECK(trace
                  .execute("SELECT p.id,q.id,z.id FROM p,q,z WHERE p.id=q.id AND visit(q.id*10+z.id)=1 AND "
                           "q.id=z.id")
                  .rows.size() == 2);
        CHECK(visited == std::vector<std::int64_t>({11, 12, 13, 21, 22, 23}));
        visited.clear();
        CHECK(trace
                  .execute("SELECT p.id,q.id,z.id FROM p,q,z WHERE visit(q.id*10+z.id)=1 AND p.id=q.id AND "
                           "q.id=z.id")
                  .rows.size() == 2);
        CHECK(visited == std::vector<std::int64_t>({11, 12, 13, 21, 22, 23, 11, 12, 13, 21, 22, 23}));
        CHECK(trace.execute("SELECT p.id FROM p ORDER BY 1+1").rows ==
              std::vector<Row>({{std::int64_t{1}}, {std::int64_t{2}}}));
        CHECK(trace.execute("SELECT count(*) FROM p GROUP BY 1+1").rows ==
              std::vector<Row>({{std::int64_t{2}}}));
        trace.execute("CREATE TABLE miss(id INTEGER)");
        trace.execute("INSERT INTO miss VALUES(99)");
        expect(ErrorCode::constraint, [&] {
            trace.execute("SELECT p.id FROM p,miss WHERE p.id='1e9999' AND p.id=miss.id AND visit(p.id)=1");
        });
        expect(ErrorCode::constraint, [&] {
            trace.execute("SELECT p.id FROM p,miss WHERE p.id='1e9999' AND p.id=miss.id");
        });
    }
});}
