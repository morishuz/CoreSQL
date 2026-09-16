#pragma once
#include "coresql/core.hpp"
#include "coresql/vector.hpp"
#include "coresql/timestamp.hpp"

// Application code, not a new engine abstraction. Embeddings are supplied by
// callers: model inference and text processing remain outside the database.
namespace example {
class DocumentIndex {
public:
    explicit DocumentIndex(const std::filesystem::path& path,
                           std::optional<std::uint64_t> create_dimensions = {})
        : db_(open(path, create_dimensions)) {
        auto schema = db_.schema();
        auto found = schema.find("documents");
        if (found == schema.end()) {
            if (!create_dimensions) throw coresql::Error(coresql::ErrorCode::schema,"No documents table; run init first");
            dimensions_ = *create_dimensions;
            auto tx = db_.begin(); tx.create_table("documents", columns(dimensions_)); tx.commit();
        } else {
            if (found->second.size() != 4) throw coresql::Error(coresql::ErrorCode::schema,"Unexpected documents schema");
            auto dimensions = coresql::vectors::dimensions(found->second[2].type);
            if (!dimensions || !*dimensions || found->second != columns(*dimensions))
                throw coresql::Error(coresql::ErrorCode::schema,"Unexpected documents schema");
            dimensions_ = *dimensions;
            if (create_dimensions && *create_dimensions != dimensions_)
                throw coresql::Error(coresql::ErrorCode::schema,"Database uses a different embedding dimension");
        }
    }
    std::uint64_t dimensions() const { return dimensions_; }
    // The engine enforces ID uniqueness and indexes these equality operations.
    bool put(std::int64_t id, std::string text, std::int64_t updated, std::span<const float> embedding) {
        using namespace coresql;
        auto vector = vectors::value(embedding, dimensions_);
        auto timestamp = timestamps::value(updated);
        auto tx = db_.begin();
        auto changed = tx.update("documents", {{"text",literal(text)}, {"embedding",literal(vector)}, {"updated",literal(timestamp)}}, by_id(id));
        if (!changed) tx.insert("documents", {id,std::move(text),std::move(vector),std::move(timestamp)});
        tx.commit(); return changed == 0;
    }
    bool erase(std::int64_t id) {
        auto tx = db_.begin(); auto changed = tx.erase("documents", by_id(id));
        tx.commit(); return changed == 1;
    }
    coresql::Result get(std::int64_t id) const {
        using namespace coresql;
        auto result = db_.query(Query{"documents", {column("id"),column("text"),column("updated")},by_id(id),{},2});
        return result;
    }
    coresql::Result list(std::size_t limit = 20, std::optional<std::int64_t> since = {},
                            std::optional<std::int64_t> until = {}) const {
        using namespace coresql;
        return db_.query(Query{"documents",{column("id"),column("text"),column("updated")},window(since,until),{Order{column("updated"),true}},limit});
    }
    coresql::Result nearest(std::span<const float> embedding, std::size_t limit = 10,
                            std::optional<std::int64_t> since = {},
                            std::optional<std::int64_t> until = {},
                            std::optional<double> max_squared_distance = {}) const {
        using namespace coresql;
        auto distance = call("vector.squared_l2",{column("embedding"),literal(vectors::value(embedding,dimensions_))});
        std::vector<Predicate> conditions;
        if (auto time = window(since,until)) conditions.push_back(std::move(*time));
        if (max_squared_distance) conditions.push_back({distance,Compare::less_equal,literal(*max_squared_distance)});
        return db_.query(Query{"documents",{column("id"),column("text"),distance,column("updated")},all_of(std::move(conditions)),{Order{distance}},limit});
    }
private:
    coresql::Database db_;
    std::uint64_t dimensions_ = 0;
    static coresql::Database open(const std::filesystem::path& path, std::optional<std::uint64_t> dimensions) {
        using namespace coresql;
        if (dimensions && !*dimensions) throw Error(ErrorCode::type,"Embedding dimension must be positive");
        if (!dimensions && !std::filesystem::exists(path)) throw Error(ErrorCode::io,"Database does not exist; run init first");
        Registry registry; vectors::install(registry); timestamps::install(registry);
        return Database::open(path,std::move(registry));
    }
    static std::vector<coresql::Column> columns(std::uint64_t n) {
        using namespace coresql;
        return {{"id",integer(),true},{"text",text()},{"embedding",vectors::type(n)},{"updated",timestamps::type()}};
    }
    static coresql::Predicate by_id(std::int64_t id) {
        return {coresql::column("id"),coresql::Compare::equal,coresql::literal(id)};
    }
    static std::optional<coresql::Predicate> window(std::optional<std::int64_t> since,
                                                     std::optional<std::int64_t> until) {
        using namespace coresql;
        std::vector<Predicate> conditions;
        if (since) conditions.push_back({column("updated"),Compare::greater,literal(timestamps::value(*since))});
        if (until) conditions.push_back({column("updated"),Compare::less_equal,literal(timestamps::value(*until))});
        if (conditions.empty()) return {};
        return all_of(std::move(conditions));
    }
};
} // namespace example
