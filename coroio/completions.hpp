#pragma once

#include "base.hpp"

#include <utility>
#include <vector>

namespace NNet {
namespace NDetail {

/**
 * @brief State of one operation of a completion-based poller (TUring, TIOCp).
 *
 * It lives in the operation's awaitable, inside the waiting coroutine's frame.
 * The poller refers to it from posting until the completion is delivered or
 * Cancel() returns, and the awaitable cancels the operation in its destructor,
 * so it outlives every reference. Index lets Cancel() find it in O(1).
 */
struct TOperationState {
    THandle Handle; ///< Coroutine to resume, set by the caller before posting. Cleared when posting fails, the completion is delivered or the operation is cancelled, so it is set while the operation is pending.
    int Result = 0; ///< Result of the completion, e.g. the number of bytes transferred.
    unsigned Index = 0; ///< Position of the completion in TCompletions once taken; before that, whatever the poller keeps there.
};

/**
 * @brief Completions taken from the system and not delivered yet.
 *
 * Ready completions are delivered by Wakeup(). Reaped ones were taken while
 * waiting for a cancelled operation and become ready at the next Carry().
 */
class TCompletions {
public:
    /// Adds a completion taken by the poller's wait.
    void Collect(TOperationState* op, int result) {
        op->Result = result;
        Add(Ready_, op);
    }

    /// Adds a completion taken while waiting for a cancelled operation.
    void Reap(TOperationState* op, int result) {
        op->Result = result;
        Add(Reaped_, op);
    }

    /// Drops the completion of a cancelled operation. Returns false if it is not taken yet.
    bool Drop(TOperationState& op) {
        // The lists hold only completions not delivered yet, so an entry that
        // points to op is its completion, whatever Index held before.
        return Remove(Ready_, op) || Remove(Reaped_, op);
    }

    /**
     * @brief Starts the next batch of ready completions.
     *
     * Keeps the ready completions not delivered, as when the poller waits
     * again without Wakeup(), and adds the reaped ones.
     *
     * @return Whether any are ready, so that the poller does not block.
     */
    bool Carry() {
        size_t size = 0;
        for (auto* op : Ready_) {
            if (op) {
                op->Index = static_cast<unsigned>(size);
                Ready_[size++] = op;
            }
        }
        Ready_.resize(size);
        for (auto* op : Reaped_) {
            if (op) {
                Add(Ready_, op);
            }
        }
        Reaped_.clear();
        return !Ready_.empty();
    }

    /// Resumes the coroutines of the ready completions.
    void Wakeup() {
        for (size_t i = 0; i < Ready_.size(); i++) {
            // Taken out before resuming: the coroutine may post its next operation
            // at the same address, and Drop() must not find this entry for it.
            if (auto* op = std::exchange(Ready_[i], nullptr)) {
                std::exchange(op->Handle, {}).resume();
            }
        }
        Ready_.clear();
    }

private:
    static void Add(std::vector<TOperationState*>& list, TOperationState* op) {
        op->Index = static_cast<unsigned>(list.size());
        list.push_back(op);
    }

    static bool Remove(std::vector<TOperationState*>& list, TOperationState& op) {
        if (op.Index < list.size() && list[op.Index] == &op) {
            list[op.Index] = nullptr;
            return true;
        }
        return false;
    }

    std::vector<TOperationState*> Ready_;
    std::vector<TOperationState*> Reaped_;
};

} // namespace NDetail
} // namespace NNet
