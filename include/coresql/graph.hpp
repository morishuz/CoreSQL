#pragma once
#include "coresql/core.hpp"
#include <type_traits>
#include <utility>

namespace coresql::graph {
using Id = std::int64_t;
struct Edge {
    Id source, target;
    std::string label;
    double weight = 1;
    bool operator==(const Edge&) const = default;
};
struct Path {
    std::vector<Id> nodes;
    std::vector<Id> edges;
    bool operator==(const Path&) const = default;
};
Type edge_type();
Type path_type();
Value value(const Edge&);
Value value(const Path&);
Edge edge(const Value&);
Path path(const Value&);
void install(Registry&);

enum class Direction { outgoing, incoming, both };
struct Filter {
    std::optional<std::string> label;
    std::optional<double> maximum_weight;
};
struct Limits {
    std::size_t vertices = 10'000;
    std::size_t edges = 100'000;
};
struct Neighbor { Id node, edge_id; Edge relationship; };
struct Visit { Id node; std::size_t depth; bool operator==(const Visit&) const = default; };

class Graph;
// Borrows a transaction. It must outlive the view and must not move while bound.
// Each failed mutation rolls back only that operation. Raw table writes bypass
// endpoint constraints; check_integrity() detects dangling edges.
class View {
public:
    View(const View&) = delete;
    View& operator=(const View&) = delete;
    View(View&& other) noexcept : transaction_(std::exchange(other.transaction_, nullptr)),
        nodes_(std::move(other.nodes_)), edges_(std::move(other.edges_)) {}
    View& operator=(View&& other) noexcept {
        if(this != &other) {
            transaction_=std::exchange(other.transaction_,nullptr);
            nodes_=std::move(other.nodes_); edges_=std::move(other.edges_);
        }
        return *this;
    }
    bool has_node(Id) const;
    void add_node(Id, std::string label = {});
    void add_edge(Id, const Edge&);
    void update_edge(Id, const Edge&);
    bool erase_edge(Id);
    bool erase_node(Id); // Cascades incident edges, including loops/parallel edges.
    std::vector<Neighbor> neighbors(Id, Direction = Direction::outgoing, Filter = {}) const;
    // Includes the start vertex at depth zero. Each vertex appears once.
    std::vector<Visit> reachable(Id, std::size_t max_hops, Direction = Direction::outgoing,
                                 Filter = {}, Limits = {}) const;
    // Fewest EDGES, not minimum weight. A missing endpoint is an error;
    // existing disconnected vertices return nullopt. Ties use ascending edge IDs.
    std::optional<Path> shortest_hop_path(Id from, Id to, std::size_t max_hops,
        Direction = Direction::outgoing, Filter = {}, Limits = {}) const;
    void check_integrity() const;
protected:
    friend class Graph;
    View(coresql::Transaction&, std::string nodes, std::string edges);
private:
    coresql::Transaction* transaction_;
    coresql::Transaction& transaction() const {
        if(!transaction_) throw Error(ErrorCode::state,"Graph view was moved from");
        return *transaction_;
    }
    std::string nodes_, edges_;
    void require_node(Id) const;
    void require_endpoints(const Edge&) const;
    template<class F> decltype(auto) write(F&& f) {
        auto point=transaction().savepoint();
        if constexpr(std::is_void_v<decltype(f())>) { f(); point.release(); }
        else { auto result=f(); point.release(); return result; }
    }
};
// Convenience owner; borrowed View deliberately has no commit/rollback methods.
class Session : public View {
public:
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    void commit();
    void rollback() noexcept { if(owned_) owned_->rollback(); }
private:
    friend class Graph;
    Session(std::unique_ptr<coresql::Transaction>, std::string nodes, std::string edges);
    std::unique_ptr<coresql::Transaction> owned_;
};
class Graph {
public:
    explicit Graph(std::string name = "graph");
    // Owns/commits its DDL transaction. Registry must already contain install().
    // Without the postings index, the same membership predicates use scan fallback.
    void create(Database&, bool indexed = true) const;
    Session begin(Database&) const;
    // DDL and graph writes participate in the caller's transaction.
    void create(coresql::Transaction&, bool indexed = true) const;
    View bind(coresql::Transaction&) const;
private:
    std::string nodes_, edges_;
};
}
