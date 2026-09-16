#include "check.hpp"
using namespace coresql;
Value n(std::int64_t v) { return v; }
Predicate key(std::int64_t v) { return {column("id"),Compare::equal,literal(n(v))}; }
int main() { return tests([] {
    TempDirectory temp;auto db=Database::open(temp.path/"db");
    auto tx=db.begin();tx.create_table("items",{{"id",integer(),true},{"label",text()}});
    tx.create_index("items",{"labels",{"label"},true});
    tx.insert("items",{n(1),std::string("one")});tx.commit();
    auto old=db.begin();tx=db.begin();tx.insert("items",{n(2),std::string("two")});
    {
        auto outer=tx.savepoint();
        tx.update("items",{{"id",literal(n(10))}},key(1));
        {auto inner=tx.savepoint();tx.erase("items",key(2));inner.release();}
        CHECK(tx.query({"items",{},key(10)}).rows.size()==1);
        tx.add_column("items",{"extra",integer(),false,{},n(5)});
        tx.create_table("temporary",{{"id",integer()}});tx.vacuum();
    }
    CHECK(tx.schema().at("items").size()==2);CHECK(!tx.schema().contains("temporary"));
    CHECK(tx.query({"items",{},key(1)}).rows.size()==1);CHECK(tx.query({"items",{},key(10)}).rows.empty());
    CHECK(tx.query({"items",{},key(2)}).rows.size()==1);tx.integrity_check();
    expect(ErrorCode::constraint,[&]{tx.insert("items",{n(3),std::string("one")});});
    {
        auto outer=tx.savepoint();tx.insert("items",{n(3),std::string("three")});
        {auto inner=tx.savepoint();tx.erase("items");inner.rollback();inner.release();}
        CHECK(tx.query({"items"}).rows.size()==3);outer.release();
    }
    // Outer rollback invalidates younger guards, even if their handles survive.
    auto outer=tx.savepoint();tx.erase("items",key(1));auto inner=tx.savepoint();tx.erase("items",key(2));
    outer.rollback();tx.insert("items",{n(4),std::string("four")});inner.rollback();
    CHECK(tx.query({"items"}).rows.size()==4);
    // Outer release also closes descendants and keeps their changes staged.
    auto release=tx.savepoint();auto child=tx.savepoint();tx.erase("items",key(4));
    release.release();child.rollback();CHECK(tx.query({"items"}).rows.size()==3);
    auto moved_scope=tx.savepoint();tx.erase("items",key(3));
    auto moved=std::move(moved_scope);auto moved_tx=std::move(tx);moved.rollback();
    CHECK(moved_tx.query({"items"}).rows.size()==3);
    expect(ErrorCode::state,[&]{(void)tx.savepoint();});
    auto guard=moved_tx.savepoint();expect(ErrorCode::state,[&]{moved_tx.commit();});guard.release();
    moved_tx.commit();CHECK(old.query({"items"}).rows.size()==1);old.rollback();
    CHECK(db.query({"items"}).rows.size()==3);
    // Restoring a clean scope must restore dirty=false: no publication or log write.
    auto clean=db.begin();auto peer=db.begin();auto stats=db.storage_stats();
    {auto scope=clean.savepoint();clean.erase("items");}
    clean.commit();CHECK(db.storage_stats().bytes_written==stats.bytes_written);
    peer.insert("items",{n(4),std::string("four")});peer.commit();
    // Scope release does not suppress conflicts or independently publish writes.
    auto stale=db.begin();auto winner=db.begin();
    {auto scope=stale.savepoint();stale.erase("items",key(1));scope.release();}
    winner.insert("items",{n(5),std::string("five")});winner.commit();
    expect(ErrorCode::conflict,[&]{stale.commit();});stale.rollback();
    // Guards outlive closed/destroyed/reassigned transactions safely.
    std::optional<Transaction::Savepoint> orphan;
    {auto local=db.begin();orphan.emplace(local.savepoint());local.erase("items");}
    orphan.reset();
    auto destination=db.begin();auto discarded=destination.savepoint();destination.erase("items");
    auto source=db.begin();auto retained=source.savepoint();source.erase("items");
    destination=std::move(source);discarded.rollback();retained.rollback();
    CHECK(destination.query({"items"}).rows.size()==5);
    auto closed=destination.savepoint();destination.rollback();closed.rollback();
    expect(ErrorCode::state,[&]{(void)destination.savepoint();});
    auto reassigned=db.begin();auto first=reassigned.savepoint();reassigned.erase("items",key(1));
    auto second=reassigned.savepoint();reassigned.erase("items",key(2));first=std::move(second);
    CHECK(reassigned.query({"items"}).rows.size()==5);first.release();reassigned.rollback();
    db=Database();db=Database::open(temp.path/"db");
    CHECK(db.query({"items"}).rows.size()==5);auto verify=db.begin();verify.integrity_check();verify.rollback();
    db.checkpoint();db=Database();db=Database::open(temp.path/"db");CHECK(db.query({"items"}).rows.size()==5);
}); }
