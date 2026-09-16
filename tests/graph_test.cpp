#include "check.hpp"
#include "coresql/graph.hpp"
#include "coresql/encoding.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <set>

using namespace coresql;
namespace g=coresql::graph;
using Model=std::map<g::Id,g::Edge>;
std::map<g::Id,std::size_t> reference(const Model& edges,g::Id start,std::size_t hops,
                                   g::Direction direction,const g::Filter& filter) {
    std::map<g::Id,std::size_t> distance{{start,0}};
    std::vector<g::Id> queue{start};
    for(std::size_t i=0;i<queue.size();++i) {
        auto node=queue[i];
        auto depth=distance.at(node);
        if(depth==hops)continue;
        for(const auto& [id,e]:edges) {
            (void)id;
            if(filter.label && e.label!=*filter.label)continue;
            if(filter.maximum_weight && e.weight>*filter.maximum_weight)continue;
            std::optional<g::Id> next;
            if(direction!=g::Direction::incoming && e.source==node)next=e.target;
            if(direction!=g::Direction::outgoing && e.target==node)next=e.source;
            if(next && distance.emplace(*next,depth+1).second)queue.push_back(*next);
        }
    }
    return distance;
}
void check_search(g::Session& tx,const Model& edges,g::Id start,g::Id target,
                  std::size_t hops,g::Direction direction={},g::Filter filter={}) {
    auto expected=reference(edges,start,hops,direction,filter);
    auto visits=tx.reachable(start,hops,direction,filter);
    std::map<g::Id,std::size_t> actual;
    for(auto visit:visits) CHECK(actual.emplace(visit.node,visit.depth).second);
    CHECK(actual==expected);
    auto path=tx.shortest_hop_path(start,target,hops,direction,filter);
    CHECK(path.has_value()==expected.contains(target));
    if(!path)return;
    CHECK(path->nodes.front()==start && path->nodes.back()==target);
    CHECK(path->edges.size()==expected.at(target));
    CHECK(path->nodes.size()==path->edges.size()+1);
    for(std::size_t i=0;i<path->edges.size();++i) {
        const auto& e=edges.at(path->edges[i]);
        const auto a=path->nodes[i],b=path->nodes[i+1];
        CHECK((direction!=g::Direction::incoming && e.source==a && e.target==b) ||
              (direction!=g::Direction::outgoing && e.target==a && e.source==b));
        CHECK(!filter.label || e.label==*filter.label);
        CHECK(!filter.maximum_weight || e.weight<=*filter.maximum_weight);
    }
}
int main(){return tests([]{
    Registry registry;g::install(registry);
    const g::Edge original{1,2,std::string("a\0b",3),3.5};
    CHECK(g::edge(g::value(original))==original);
    CHECK(registry.equal(g::value(g::Edge{1,2,"",-0.0}),g::value(g::Edge{1,2,"",0.0})));
    auto negative_zero=g::value(g::Edge{1,2,"",0.0});
    auto zero_bytes=std::get<Opaque>(negative_zero).bytes();
    Bytes noncanonical(zero_bytes.begin(),zero_bytes.end()); noncanonical[23]=std::byte{0x80};
    expect(ErrorCode::format,[&]{g::edge(Opaque(g::edge_type(),noncanonical));});
    const g::Path route{{1,2,3},{7,8}};
    CHECK(g::path(g::value(route))==route);
    CHECK(g::path(g::value(g::Path{{1},{}})).nodes==std::vector<g::Id>{1});
    expect(ErrorCode::constraint,[&]{g::value(g::Edge{0,2,"",1});});
    expect(ErrorCode::constraint,[&]{g::value(g::Edge{1,2,"",INFINITY});});
    expect(ErrorCode::constraint,[&]{g::value(g::Path{{1,2},{}});});
    expect(ErrorCode::type,[&]{g::edge(Value(std::int64_t{1}));});
    for(auto v:{g::value(original),g::value(route)}) {
        auto bytes=std::get<Opaque>(v).bytes();
        for(std::size_t n=0;n<bytes.size();++n)
            expect(ErrorCode::format,[&]{registry.validate(Opaque(type_of(v),Bytes(bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(n))),type_of(v));});
        Bytes trailing(bytes.begin(),bytes.end());trailing.push_back(std::byte{0});
        expect(ErrorCode::format,[&]{registry.validate(Opaque(type_of(v),trailing),type_of(v));});
    }
    Bytes enormous;encoding::u64(enormous,UINT64_MAX);
    expect(ErrorCode::format,[&]{registry.validate(Opaque(g::path_type(),enormous),g::path_type());});
    Database missing;
    expect(ErrorCode::type,[&]{g::Graph{}.create(missing);});CHECK(missing.schema().empty());
    // Deterministic randomized oracle: indexed and scan fallback must agree with
    // an independent in-memory model for direction, filtering, hops and paths.
    for(bool indexed:{false,true}) {
        Database db(registry);g::Graph graph;graph.create(db,indexed);
        auto tx=graph.begin(db);Model edges;std::mt19937 rng(751);
        for(g::Id id=1;id<=60;++id)tx.add_node(id);
        for(g::Id id=1;id<=240;++id) {
            g::Edge e{static_cast<g::Id>(rng()%60+1),static_cast<g::Id>(rng()%60+1),rng()%2?"a":"b",double(rng()%5)};
            tx.add_edge(id,e);edges.emplace(id,e);
        }
        tx.commit();auto read=graph.begin(db);
        for(int trial=0;trial<24;++trial)for(auto direction:{g::Direction::outgoing,g::Direction::incoming,g::Direction::both}) {
            g::Filter filter;
            if(trial%2)filter.label="a";
            if(trial%3)filter.maximum_weight=2;
            check_search(read,edges,static_cast<g::Id>(rng()%60+1),static_cast<g::Id>(rng()%60+1),static_cast<std::size_t>(trial%5),direction,filter);
        }
        read.check_integrity();
    }
    TempDirectory temp;
    auto db=Database::open(temp.path/"graph",registry);g::Graph graph("social");graph.create(db);
    auto tx=graph.begin(db);
    for(g::Id id=1;id<=5;++id)tx.add_node(id);
    tx.add_edge(10,{1,2,"friend",1});tx.add_edge(11,{2,3,"friend",1});
    tx.add_edge(12,{3,1,"friend",1});tx.add_edge(13,{1,2,"friend",2});tx.add_edge(14,{1,1,"loop",0});
    tx.commit();
    auto removal=graph.begin(db);CHECK(removal.erase_edge(10));CHECK(!removal.erase_edge(10));
    CHECK(removal.neighbors(1).size()==2);removal.rollback();
    auto old=graph.begin(db);
    auto p=old.shortest_hop_path(1,3,2);CHECK(p && p->edges==std::vector<g::Id>({10,11}));
    CHECK(old.neighbors(1,g::Direction::both).size()==4); // Self-loop appears once.
    CHECK(!old.shortest_hop_path(1,5,10));
    CHECK(old.shortest_hop_path(1,1,0)->edges.empty());
    expect(ErrorCode::constraint,[&]{old.reachable(1,3,g::Direction::outgoing,{},g::Limits{1,100});});
    expect(ErrorCode::constraint,[&]{old.reachable(1,3,g::Direction::outgoing,{},g::Limits{100,0});});
    CHECK(old.has_node(1)); // Read error leaves session usable.
    expect(ErrorCode::type,[&]{old.neighbors(1,static_cast<g::Direction>(77));});
    tx=graph.begin(db);tx.update_edge(10,{1,4,"other",0.5});tx.commit();
    CHECK(old.shortest_hop_path(1,3,2)->edges==std::vector<g::Id>({10,11}));
    auto changed=graph.begin(db);CHECK(changed.shortest_hop_path(1,4,1)->edges==std::vector<g::Id>{10});
    CHECK(changed.neighbors(2,g::Direction::incoming).size()==1);changed.rollback();
    tx=graph.begin(db);CHECK(tx.erase_node(1));tx.rollback();CHECK(graph.begin(db).has_node(1));
    tx=graph.begin(db);CHECK(tx.erase_node(1));tx.commit();
    auto after=graph.begin(db);after.check_integrity();CHECK(after.neighbors(2).size()==1);CHECK(after.neighbors(3,g::Direction::both).size()==1);
    CHECK(old.neighbors(1).size()==3);old.rollback();after.rollback();
    // A failed mutation preserves prior graph writes and leaves the session usable.
    tx=graph.begin(db);tx.add_node(99);
    expect(ErrorCode::constraint,[&]{tx.add_edge(90,{99,999,"bad",1});});
    CHECK(tx.has_node(99));tx.commit();CHECK(graph.begin(db).has_node(99));
    // Concurrent snapshots cannot publish an edge after another session deletes
    // its endpoint: core OCC conflicts; the caller can inspect and roll back.
    auto a=graph.begin(db),b=graph.begin(db);a.add_edge(91,{2,4,"x",1});b.erase_node(4);b.commit();
    expect(ErrorCode::conflict,[&]{a.commit();});a.rollback();graph.begin(db).check_integrity();
    // Store a path as an ordinary custom value; scalar binding and persistence
    // require only the registry, independently of a live graph's current edges.
    auto raw=db.begin();raw.create_table("routes",{{"path",g::path_type()}});raw.insert("routes",{g::value(route)});raw.commit();
    CHECK(std::get<g::Id>(db.query({"routes",{call("graph.path.hops",{column("path")})}}).rows[0][0])==2);
    auto verify=[&](Database& database){auto s=graph.begin(database);s.check_integrity();CHECK(!s.has_node(1));CHECK(!s.has_node(4));CHECK(s.shortest_hop_path(2,3,1)->edges==std::vector<g::Id>{11});CHECK(g::path(database.query({"routes",{}}).rows[0][0])==route);};
    db.save(temp.path/"snapshot");auto imported=Database::load(temp.path/"snapshot",registry);verify(imported);
    expect(ErrorCode::type,[&]{Database::load(temp.path/"snapshot");});
    db=Database();db=Database::open(temp.path/"graph",registry);verify(db);db.checkpoint();db=Database();db=Database::open(temp.path/"graph",registry);verify(db);
    // Ordinary writes, graph DDL and graph operations share a caller transaction.
    Database composed(registry);auto app=composed.begin();
    app.create_table("audit",{{"id",integer(),true}});app.insert("audit",{g::Id{1}});
    g::Graph joined("joined");joined.create(app);auto view=joined.bind(app);
    view.add_node(1);view.add_node(2);view.add_edge(1,{1,2,"",1});
    expect(ErrorCode::constraint,[&]{view.add_edge(2,{1,999,"",1});});
    CHECK(app.query({"audit"}).rows.size()==1);CHECK(view.neighbors(1).size()==1);
    {auto scope=app.savepoint();view.erase_node(1);app.insert("audit",{g::Id{2}});}
    CHECK(view.has_node(1));CHECK(view.neighbors(1).size()==1);CHECK(app.query({"audit"}).rows.size()==1);
    app.commit();CHECK(joined.begin(composed).neighbors(1).size()==1);
    expect(ErrorCode::state,[&]{view.has_node(1);});
    app=composed.begin();
    g::Graph clash("clash");app.create_table("clash.edges",{{"id",integer()}});
    expect(ErrorCode::schema,[&]{clash.create(app);});
    CHECK(!app.schema().contains("clash.nodes"));CHECK(app.schema().contains("clash.edges"));app.rollback();
    // Inject a failure in the SECOND cascade mutation: the node's ordered index
    // throws after incident edges have already been removed from staged state.
    bool fail_delete=false;Registry fault_registry(false);
    fault_registry.add(registry.addon(integer()));fault_registry.add(registry.addon(real()));
    auto text_addon=registry.addon(text());auto compare=text_addon.compare;
    text_addon.compare=[&](ByteView p,const Value& x,const Value& y){
        if(fail_delete)throw Error(ErrorCode::type,"Injected node index failure");
        return compare(p,x,y);
    };
    fault_registry.add(std::move(text_addon));g::install(fault_registry);
    Database faulty(fault_registry);g::Graph cascade;cascade.create(faulty);
    auto setup=cascade.begin(faulty);setup.add_node(1,"one");setup.add_node(2,"two");setup.add_edge(10,{1,2,"",1});setup.commit();
    auto enclosing=faulty.begin();enclosing.create_index("graph.nodes",{"labels",{"label"}});
    enclosing.create_table("prior",{{"id",integer()}});enclosing.insert("prior",{g::Id{42}});
    auto graph_view=cascade.bind(enclosing);fail_delete=true;
    expect(ErrorCode::type,[&]{graph_view.erase_node(1);});fail_delete=false;
    CHECK(graph_view.has_node(1));CHECK(graph_view.neighbors(1).size()==1);
    CHECK(graph_view.neighbors(2,g::Direction::incoming).size()==1);
    CHECK(enclosing.query({"prior"}).rows.size()==1);graph_view.check_integrity();enclosing.commit();
    // High-degree hub and budgets, with a disconnected vertex.
    Database hub(registry);g::Graph star;star.create(hub);auto h=star.begin(hub);
    for(g::Id i=1;i<=514;++i)h.add_node(i);
    for(g::Id i=2;i<=513;++i)h.add_edge(i,{1,i,"spoke",1});h.commit();
    h=star.begin(hub);CHECK(h.neighbors(1).size()==512);CHECK(h.reachable(1,1).size()==513);
    CHECK(!h.shortest_hop_path(1,514,2));
    expect(ErrorCode::constraint,[&]{h.reachable(1,1,g::Direction::outgoing,{},g::Limits{100,1000});});
    // Exposed boundary: raw core writes bypass graph-level endpoint constraints.
    auto bypass=hub.begin();bypass.insert("graph.edges",{g::Id{900},g::value(g::Edge{1,999,"orphan",1})});bypass.commit();
    expect(ErrorCode::constraint,[&]{star.begin(hub).check_integrity();});
});}
