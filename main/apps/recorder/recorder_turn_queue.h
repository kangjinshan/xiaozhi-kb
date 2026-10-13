#ifndef RECORDER_TURN_QUEUE_H_
#define RECORDER_TURN_QUEUE_H_

#include "agent_turn_store.h"
#include <deque>

// Load the on-card pending scan only at startup. Scanning while a reply is being
// received would recover/delete its live .part file. New recordings append here.
class RecorderTurnQueue {
public:
    bool empty() const { return turns_.empty(); }
    size_t size() const { return turns_.size(); }
    const AgentPendingTurn& active() const { return turns_.front(); }
    void Push(const AgentPendingTurn& turn) {
        for (const auto& existing : turns_) {
            if (existing.paths.turn_id == turn.paths.turn_id) return;
        }
        turns_.push_back(turn);
    }
    void FinishActive() { if (!empty()) turns_.pop_front(); }

private:
    std::deque<AgentPendingTurn> turns_;
};

#endif
