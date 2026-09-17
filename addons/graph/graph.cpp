#include "coresql/graph.hpp"
#include "coresql/encoding.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>

namespace coresql::graph {
namespace {
constexpr const char* endpoints = "graph.endpoints.v1";
Type endpoint_type() { return {"graph.endpoint", 1, {}}; }
void id(Id n) {
    if (n <= 0) throw Error(ErrorCode::constraint, "Graph IDs must be positive");
}
void valid(const Edge& e) {
    id(e.source); id(e.target);
    if (!std::isfinite(e.weight) || e.weight < 0)
        throw Error(ErrorCode::constraint, "Edge weight must be finite and nonnegative");
}
ByteView payload(const Value& v, const Type& t) {
    const auto* p = std::get_if<Opaque>(&v);
    if (!p || p->type() != t) throw Error(ErrorCode::type, "Wrong graph value type");
    return p->bytes();
}
Id read_id(encoding::Reader& r) {
    auto n = std::bit_cast<Id>(r.u64());
    if (n <= 0) throw Error(ErrorCode::format, "Invalid encoded graph ID");
    return n;
}
Edge decode_edge(ByteView bytes) {
    encoding::Reader r(bytes);
    Edge e{read_id(r), read_id(r), {}, std::bit_cast<double>(r.u64())};
    if (!std::isfinite(e.weight) || std::signbit(e.weight))
        throw Error(ErrorCode::format, "Invalid encoded edge weight");
    auto length = r.u64();
    if (length > r.remaining()) throw Error(ErrorCode::format, "Invalid edge label length");
    auto label = r.take(static_cast<std::size_t>(length));
    e.label.assign(reinterpret_cast<const char*>(label.data()), label.size());
    r.end(); return e;
}
Path decode_path(ByteView bytes) {
    encoding::Reader r(bytes);
    auto count = r.u64();
    if (!count || count > r.remaining()/8 || r.remaining()%8 || r.remaining()/8 != 2*count-1)
        throw Error(ErrorCode::format, "Invalid encoded path length");
    Path p;
    for (std::uint64_t i=0; i<count; ++i) p.nodes.push_back(read_id(r));
    for (std::uint64_t i=1; i<count; ++i) p.edges.push_back(read_id(r));
    r.end(); return p;
}
Value endpoint(Id node, bool incoming) {
    id(node);
    Bytes bytes{std::byte(incoming)};
    encoding::u64(bytes, static_cast<std::uint64_t>(node));
    return Opaque(endpoint_type(), std::move(bytes));
}
Predicate membership(Id node, bool incoming) {
    return contains(column("edge"), endpoints, literal(endpoint(node, incoming)));
}
Predicate key(Id n) { return {column("id"), Compare::equal, literal(Value(n))}; }
void validate(Direction direction, const Filter& filter) {
    if (direction != Direction::outgoing && direction != Direction::incoming && direction != Direction::both)
        throw Error(ErrorCode::type, "Unknown graph direction");
    if (filter.maximum_weight && (!std::isfinite(*filter.maximum_weight) || *filter.maximum_weight < 0))
        throw Error(ErrorCode::constraint, "Weight filter must be finite and nonnegative");
}
std::vector<Column> node_schema() { return {{"id",integer(),true},{"label",text()}}; }
std::vector<Column> edge_schema(bool indexed) {
    return {{"id",integer(),true},{"edge",edge_type(),false,indexed ? endpoints : ""}};
}
struct Search {
    std::vector<Visit> visits;
    std::map<Id,std::pair<Id,Id>> parents;
};
Search search(const View& session, Id start, std::optional<Id> target,
              std::size_t hops, Direction direction, const Filter& filter, Limits limits) {
    validate(direction, filter);
    if (!session.has_node(start) || (target && !session.has_node(*target)))
        throw Error(ErrorCode::constraint, "Traversal endpoint does not exist");
    if (!limits.vertices) throw Error(ErrorCode::constraint, "Traversal vertex budget exceeded");
    Search result{{{start,0}},{{start,{start,0}}}};
    if (target == start) return result;
    std::size_t examined = 0;
    for (std::size_t i=0; i<result.visits.size(); ++i) {
        auto current = result.visits[i];
        if (current.depth == hops) continue;
        // The current core API materializes this adjacency result before the
        // traversal can enforce its edge budget; this is not a heap-memory cap.
        for (const auto& link : session.neighbors(current.node, direction, filter)) {
            if (examined == limits.edges) throw Error(ErrorCode::constraint, "Traversal edge budget exceeded");
            ++examined;
            if (result.parents.contains(link.node)) continue;
            if (result.visits.size() == limits.vertices)
                throw Error(ErrorCode::constraint, "Traversal vertex budget exceeded");
            result.parents.emplace(link.node, std::pair{current.node,link.edge_id});
            result.visits.push_back({link.node,current.depth+1});
            if (target == link.node) return result;
        }
    }
    return result;
}
}
Type edge_type() { return {"graph.edge",1,{}}; }
Type path_type() { return {"graph.path",1,{}}; }
Value value(const Edge& e) {
    valid(e);
    Bytes bytes;
    encoding::u64(bytes, static_cast<std::uint64_t>(e.source));
    encoding::u64(bytes, static_cast<std::uint64_t>(e.target));
    encoding::u64(bytes, std::bit_cast<std::uint64_t>(e.weight == 0 ? 0.0 : e.weight));
    encoding::u64(bytes, e.label.size());
    auto label = std::as_bytes(std::span(e.label.data(),e.label.size()));
    bytes.insert(bytes.end(),label.begin(),label.end());
    return Opaque(edge_type(),std::move(bytes));
}
Value value(const Path& p) {
    if (p.nodes.empty() || p.edges.size() != p.nodes.size()-1)
        throw Error(ErrorCode::constraint, "Path needs one more vertex than edge");
    Bytes bytes; encoding::u64(bytes,p.nodes.size());
    for (auto n:p.nodes) { id(n); encoding::u64(bytes,static_cast<std::uint64_t>(n)); }
    for (auto n:p.edges) { id(n); encoding::u64(bytes,static_cast<std::uint64_t>(n)); }
    return Opaque(path_type(),std::move(bytes));
}
Edge edge(const Value& v) { return decode_edge(payload(v,edge_type())); }
Path path(const Value& v) { return decode_path(payload(v,path_type())); }
void install(Registry& registry) {
    auto register_type = [&](Type t, auto check) {
        EncodedTypeAddon a;
        a.id = t.id;
        a.validate_type = [](ByteView p) { if (!p.empty()) throw Error(ErrorCode::type,"Graph types take no parameters"); };
        a.validate_value = [check](ByteView,ByteView b) { check(b); };
        // Equality is exact encoded value equality, not graph isomorphism.
        a.equal = [](ByteView,ByteView a,ByteView b) { return std::ranges::equal(a,b); };
        return encoded_type(std::move(a));
    };
    registry.add(register_type(edge_type(),decode_edge));
    registry.add(register_type(path_type(),decode_path));
    auto key_type = register_type(endpoint_type(),[](ByteView bytes) {
        if (bytes.size()!=9 || std::to_integer<unsigned>(bytes[0])>1)
            throw Error(ErrorCode::format,"Invalid graph endpoint key");
        encoding::Reader r(bytes.subspan(1)); read_id(r); r.end();
    });
    key_type.hash = [](ByteView,const Value& v) {
        std::size_t h=0;
        for(auto b:payload(v,endpoint_type())) h=h*131+std::to_integer<unsigned>(b);
        return h;
    };
    // No ordering is needed for an extracted hash key.
    registry.add(std::move(key_type));
    registry.add(KeyExtractor{endpoints,edge_type(),endpoint_type(),[](const Value& v) {
        auto e=edge(v); return std::vector<Value>{endpoint(e.source,false),endpoint(e.target,true)};
    }});
    auto scalar = [&](std::string name,Type input,Type output,auto invoke) {
        registry.add(Function{std::move(name),[input,output](std::span<const Type> types) {
            if(types.size()!=1 || types[0]!=input) throw Error(ErrorCode::type,"Wrong graph scalar argument");
            return output;
        },[invoke](std::span<const Value> values)->Value { return invoke(values[0]); }});
    };
    scalar("graph.edge.source",edge_type(),integer(),[](const Value& v) { return edge(v).source; });
    scalar("graph.edge.target",edge_type(),integer(),[](const Value& v) { return edge(v).target; });
    scalar("graph.edge.label",edge_type(),text(),[](const Value& v) { return edge(v).label; });
    scalar("graph.edge.weight",edge_type(),real(),[](const Value& v) { return edge(v).weight; });
    scalar("graph.path.hops",path_type(),integer(),[](const Value& v) { return static_cast<Id>(path(v).edges.size()); });
}
Graph::Graph(std::string name) {
    if(name.empty()) throw Error(ErrorCode::schema,"Graph needs a name");
    nodes_=name+".nodes"; edges_=name+".edges";
}
void Graph::create(Database& db,bool indexed) const {
    auto tx=db.begin(); create(tx,indexed); tx.commit();
}
void Graph::create(coresql::Transaction& tx,bool indexed) const {
    auto point=tx.savepoint();
    tx.create_table(nodes_,node_schema()); tx.create_table(edges_,edge_schema(indexed));
    point.release();
}
Session Graph::begin(Database& db) const {
    return Session(std::make_unique<coresql::Transaction>(db.begin()),nodes_,edges_);
}
View Graph::bind(coresql::Transaction& tx) const { return View(tx,nodes_,edges_); }
Session::Session(std::unique_ptr<coresql::Transaction> tx,std::string nodes,std::string edges)
    : View(*tx,std::move(nodes),std::move(edges)),owned_(std::move(tx)) {}
View::View(coresql::Transaction& tx,std::string nodes,std::string edges)
    : transaction_(&tx),nodes_(std::move(nodes)),edges_(std::move(edges)) {
    const auto schema=transaction().schema();
    auto n=schema.find(nodes_),e=schema.find(edges_);
    if(n==schema.end() || e==schema.end() || n->second!=node_schema() ||
       (e->second!=edge_schema(true) && e->second!=edge_schema(false)))
        throw Error(ErrorCode::schema,"Graph schema does not match this extension version");
}
bool View::has_node(Id node) const {
    id(node); return !transaction().query({nodes_,{column("id")},key(node),{},1}).rows.empty();
}
void View::require_node(Id node) const {
    if(!has_node(node)) throw Error(ErrorCode::constraint,"Graph vertex does not exist");
}
void View::require_endpoints(const Edge& e) const { valid(e); require_node(e.source); require_node(e.target); }
void View::add_node(Id node,std::string label) {
    write([&] { id(node); transaction().insert(nodes_,{node,std::move(label)}); });
}
void View::add_edge(Id edge_id,const Edge& e) {
    write([&] { id(edge_id); require_endpoints(e); transaction().insert(edges_,{edge_id,value(e)}); });
}
void View::update_edge(Id edge_id,const Edge& e) {
    write([&] {
        id(edge_id); require_endpoints(e);
        if(!transaction().update(edges_,{{"edge",literal(value(e))}},key(edge_id)))
            throw Error(ErrorCode::constraint,"Graph edge does not exist");
    });
}
bool View::erase_edge(Id edge_id) {
    return write([&] { id(edge_id); return transaction().erase(edges_,key(edge_id))!=0; });
}
bool View::erase_node(Id node) {
    return write([&] {
        if(!has_node(node)) return false;
        transaction().erase(edges_,any_of({membership(node,false),membership(node,true)}));
        return transaction().erase(nodes_,key(node))!=0;
    });
}
std::vector<Neighbor> View::neighbors(Id node,Direction direction,Filter filter) const {
    validate(direction,filter); require_node(node);
    std::vector<Neighbor> result;
    for(bool incoming:{false,true}) {
        if((direction==Direction::outgoing && incoming) || (direction==Direction::incoming && !incoming)) continue;
        std::vector<Predicate> predicates{membership(node,incoming)};
        if(filter.label) predicates.push_back({call("graph.edge.label",{column("edge")}),Compare::equal,literal(*filter.label)});
        if(filter.maximum_weight) predicates.push_back({call("graph.edge.weight",{column("edge")}),Compare::less_equal,literal(*filter.maximum_weight)});
        auto rows=transaction().query({edges_,{},all_of(std::move(predicates)),{Order{column("id")}}});
        for(const auto& row:rows.rows) {
            auto e=edge(row[1]);
            result.push_back({incoming ? e.source : e.target,std::get<Id>(row[0]),std::move(e)});
        }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){ return a.edge_id<b.edge_id; });
    result.erase(std::unique(result.begin(),result.end(),[](const auto& a,const auto& b){return a.edge_id==b.edge_id;}),result.end());
    return result;
}
std::vector<Visit> View::reachable(Id start,std::size_t hops,Direction direction,Filter filter,Limits limits) const {
    return search(*this,start,{},hops,direction,filter,limits).visits;
}
std::optional<Path> View::shortest_hop_path(Id from,Id to,std::size_t hops,Direction direction,Filter filter,Limits limits) const {
    auto found=search(*this,from,to,hops,direction,filter,limits);
    if(!found.parents.contains(to)) return {};
    Path p;
    for(Id node=to;;) {
        p.nodes.push_back(node); if(node==from) break;
        auto [parent,edge_id]=found.parents.at(node); p.edges.push_back(edge_id); node=parent;
    }
    std::reverse(p.nodes.begin(),p.nodes.end()); std::reverse(p.edges.begin(),p.edges.end()); return p;
}
void View::check_integrity() const {
    transaction().integrity_check();
    std::set<Id> nodes;
    for(const auto& row:transaction().query({nodes_,{column("id")},{}}).rows) { auto n=std::get<Id>(row[0]); id(n); nodes.insert(n); }
    for(const auto& row:transaction().query({edges_,{},{}}).rows) {
        id(std::get<Id>(row[0])); auto e=edge(row[1]);
        if(!nodes.contains(e.source) || !nodes.contains(e.target)) throw Error(ErrorCode::constraint,"Dangling graph edge");
    }
}
void Session::commit() {
    if(!owned_) throw Error(ErrorCode::state,"Graph session was moved from");
    owned_->commit();
}
}
