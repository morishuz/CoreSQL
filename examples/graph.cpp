#include "coresql/graph.hpp"
#include <iostream>

int main() {
    try {
        using namespace coresql;
        Registry registry; graph::install(registry);
        Database db(registry); graph::Graph graph("example");
        auto app=db.begin();
        app.create_table("audit",{{"message",text()}});
        graph.create(app);
        auto tx=graph.bind(app);
        for(graph::Id id=1;id<=5;++id) tx.add_node(id,"vertex "+std::to_string(id));
        tx.add_edge(10,{1,2,"friend",1});
        tx.add_edge(11,{2,3,"friend",2});
        tx.add_edge(12,{3,1,"friend",1}); // Cycle.
        tx.add_edge(13,{1,2,"colleague",3}); // Parallel edge, different label.
        tx.add_edge(14,{3,4,"friend",1});
        app.insert("audit",{std::string("Created example graph")});
        app.commit(); // Graph and ordinary application writes publish together.
        auto read=graph.begin(db);
        std::cout<<"Friend vertices reachable within two hops from 1:";
        for(auto visit:read.reachable(1,2,graph::Direction::outgoing,{{"friend"},{}}))
            std::cout<<' '<<visit.node<<"(depth="<<visit.depth<<')';
        auto route=read.shortest_hop_path(1,4,3,graph::Direction::outgoing,{{"friend"},{}});
        if(!route) throw std::runtime_error("Expected route");
        std::cout<<"\nShortest hop path:";
        for(auto node:route->nodes)std::cout<<' '<<node;
        std::cout<<"\nEdge IDs:";for(auto edge:route->edges)std::cout<<' '<<edge;
        std::cout<<'\n';
        read.check_integrity();read.rollback();
        // A path is also a regular custom CoreSQL value, independently of Graph.
        auto store=db.begin();store.create_table("routes",{{"path",graph::path_type()}});
        store.insert("routes",{graph::value(*route)});store.commit();
        auto lengths=db.query({"routes",{call("graph.path.hops",{column("path")})}});
        std::cout<<"Stored path queried through registered scalar: "
                 <<std::get<std::int64_t>(lengths.rows[0][0])<<" hops\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
