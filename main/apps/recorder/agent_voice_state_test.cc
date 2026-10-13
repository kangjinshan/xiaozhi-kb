#include "agent_voice_state.h"

#include <cassert>
#include <cstring>

int main() {
    AgentVoiceState state;
    assert(state.phase == AgentVoicePhase::kOffline);
    assert(AgentVoiceCanRecord(state));

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kWifiConnected) ==
           AgentVoiceAction::kConnectSocket);
    assert(state.phase == AgentVoicePhase::kConnecting);

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kTurnQueued) ==
           AgentVoiceAction::kNone);
    assert(state.queued_turn);

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kServerReady) ==
           AgentVoiceAction::kSendQueuedTurn);
    assert(state.phase == AgentVoicePhase::kSending);
    assert(AgentVoiceCanRecord(state));

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kTurnAccepted) ==
           AgentVoiceAction::kWaitForReply);
    assert(state.phase == AgentVoicePhase::kThinking);

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kReplyStarted) ==
           AgentVoiceAction::kReceiveReply);
    assert(state.phase == AgentVoicePhase::kReceiving);

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kReplyStored) ==
           AgentVoiceAction::kPlayReply);
    assert(state.phase == AgentVoicePhase::kOnline);
    assert(!state.queued_turn);

    // Playback is independent; finishing old audio cannot clear the next upload.
    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kTurnQueued) ==
           AgentVoiceAction::kSendQueuedTurn);
    AgentVoiceReduce(&state, AgentVoiceEvent::kPlaybackStarted);
    AgentVoiceReduce(&state, AgentVoiceEvent::kPlaybackFinished);
    assert(state.phase == AgentVoicePhase::kSending && state.queued_turn);

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kDisconnected) ==
           AgentVoiceAction::kScheduleReconnect);
    assert(state.phase == AgentVoicePhase::kOffline);
    assert(std::strcmp(AgentVoicePhaseTitle(state.phase), "OFFLINE") == 0);

    state.queued_turn = true;
    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kServerReady) ==
           AgentVoiceAction::kSendQueuedTurn);
    assert(state.phase == AgentVoicePhase::kSending);

    assert(AgentVoiceReduce(&state, AgentVoiceEvent::kTurnRejected) ==
           AgentVoiceAction::kNone);
    assert(state.phase == AgentVoicePhase::kOnline);
    assert(!state.queued_turn);
    assert(AgentVoiceCanRecord(state));

    return 0;
}
