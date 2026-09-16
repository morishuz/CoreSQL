#include "storage.hpp"

namespace coresql {
struct Transaction::SavepointState {
    Transaction* owner;
    std::unique_ptr<detail::State> snapshot;
    bool dirty;
    std::shared_ptr<SavepointState> parent;
};
Transaction::Savepoint::Savepoint(std::shared_ptr<SavepointState> state) : state_(std::move(state)) {}
Transaction::Savepoint::Savepoint(Savepoint&&) noexcept = default;
Transaction::Savepoint& Transaction::Savepoint::operator=(Savepoint&& other) noexcept {
    if (this != &other) { rollback(); state_ = std::move(other.state_); }
    return *this;
}
Transaction::Savepoint::~Savepoint() { rollback(); }
void Transaction::Savepoint::rollback() noexcept {
    if (state_ && state_->owner) state_->owner->finish_savepoint(state_.get(), true);
}
void Transaction::Savepoint::rollback_to() {
    if (!state_ || !state_->owner)
        throw Error(ErrorCode::state, "Savepoint is closed");
    auto* owner = state_->owner;
    owner->active();
    // Allocate before changing staged state so failure preserves the transaction.
    auto replacement = std::make_shared<SavepointState>(SavepointState{
        owner, std::make_unique<detail::State>(*state_->snapshot), state_->dirty, state_->parent});
    owner->finish_savepoint(state_.get(), true);
    owner->savepoint_ = replacement;
    state_ = std::move(replacement);
}
void Transaction::Savepoint::release() noexcept {
    if (state_ && state_->owner) state_->owner->finish_savepoint(state_.get(), false);
}
Transaction::Savepoint Transaction::savepoint() {
    active();
    auto state = std::make_shared<SavepointState>(SavepointState{
        this, std::make_unique<detail::State>(*staged_), dirty_, savepoint_});
    savepoint_ = state;
    return Savepoint(std::move(state));
}
void Transaction::finish_savepoint(SavepointState* target, bool restore) noexcept {
    if (restore) { staged_ = std::move(target->snapshot); dirty_ = target->dirty; }
    // All descendants belong to this scope, including released inner writes.
    while (savepoint_) {
        auto point = std::move(savepoint_);
        savepoint_ = std::move(point->parent);
        point->owner = nullptr;
        point->snapshot.reset();
        if (point.get() == target) break;
    }
}
void Transaction::retarget_savepoints() noexcept {
    for (auto point = savepoint_; point; point = point->parent) point->owner = this;
}
Transaction::Transaction(const std::shared_ptr<detail::Owner>& owner)
    : owner_(owner), base_(owner->current), staged_(std::make_unique<detail::State>(*base_)) {}
Transaction::~Transaction() { rollback(); }
Transaction::Transaction(Transaction&& other) noexcept
    : owner_(std::move(other.owner_)), base_(std::move(other.base_)), staged_(std::move(other.staged_)),
      dirty_(other.dirty_), savepoint_(std::move(other.savepoint_)) { retarget_savepoints(); }
Transaction& Transaction::operator=(Transaction&& other) noexcept {
    if (this != &other) {
        rollback();
        owner_ = std::move(other.owner_); base_ = std::move(other.base_);
        staged_ = std::move(other.staged_); dirty_ = other.dirty_;
        savepoint_ = std::move(other.savepoint_); retarget_savepoints();
    }
    return *this;
}

std::shared_ptr<detail::Owner> Transaction::active() const {
    auto owner = owner_.lock();
    if (!owner || !staged_) throw Error(ErrorCode::state, "Transaction is closed or database no longer exists");
    detail::healthy(owner);
    return owner;
}
void Transaction::commit() {
    auto owner = active();
    if (savepoint_) throw Error(ErrorCode::state, "Release or roll back savepoints before committing");
    if (owner->current != base_) throw Error(ErrorCode::conflict, "Database changed since transaction began; rollback and retry");
    if (!dirty_) { rollback(); return; }
    staged_->stats = detail::measure(*staged_);
    // Allocate before consuming staged state so allocation failure leaves it usable.
    auto next = std::make_shared<detail::State>(*staged_);
    if (owner->storage) owner->storage->commit(*base_, *next);
    owner->current = std::move(next);
    rollback();
}
void Transaction::rollback() noexcept {
    if (savepoint_) finish_savepoint(nullptr, false);
    staged_.reset(); base_.reset(); owner_.reset(); dirty_ = false;
}

} // namespace coresql
