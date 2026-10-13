#include "agent_voice_state.h"

AgentVoiceAction AgentVoiceReduce(AgentVoiceState* state, AgentVoiceEvent event) {
    if (state == nullptr) {
        return AgentVoiceAction::kNone;
    }
    switch (event) {
        case AgentVoiceEvent::kWifiConnected:
            state->phase = AgentVoicePhase::kConnecting;
            return AgentVoiceAction::kConnectSocket;
        case AgentVoiceEvent::kServerReady:
            if (state->queued_turn) {
                state->phase = AgentVoicePhase::kSending;
                return AgentVoiceAction::kSendQueuedTurn;
            }
            state->phase = AgentVoicePhase::kOnline;
            return AgentVoiceAction::kNone;
        case AgentVoiceEvent::kTurnQueued:
            state->queued_turn = true;
            if (state->phase == AgentVoicePhase::kOnline) {
                state->phase = AgentVoicePhase::kSending;
                return AgentVoiceAction::kSendQueuedTurn;
            }
            return AgentVoiceAction::kNone;
        case AgentVoiceEvent::kTurnAccepted:
            state->phase = AgentVoicePhase::kThinking;
            return AgentVoiceAction::kWaitForReply;
        case AgentVoiceEvent::kTurnRejected:
            state->phase = AgentVoicePhase::kOnline;
            state->queued_turn = false;
            return AgentVoiceAction::kNone;
        case AgentVoiceEvent::kReplyStarted:
            state->phase = AgentVoicePhase::kReceiving;
            return AgentVoiceAction::kReceiveReply;
        case AgentVoiceEvent::kReplyStored:
            state->phase = AgentVoicePhase::kOnline;
            state->queued_turn = false;
            return AgentVoiceAction::kPlayReply;
        // Audio playback must never modify the transport's active turn.
        case AgentVoiceEvent::kPlaybackStarted:
        case AgentVoiceEvent::kPlaybackFinished:
            return AgentVoiceAction::kNone;
        case AgentVoiceEvent::kDisconnected:
            state->phase = AgentVoicePhase::kOffline;
            return AgentVoiceAction::kScheduleReconnect;
        case AgentVoiceEvent::kFailure:
            state->phase = AgentVoicePhase::kError;
            return AgentVoiceAction::kScheduleReconnect;
    }
    return AgentVoiceAction::kNone;
}

bool AgentVoiceCanRecord(const AgentVoiceState&) {
    // Recordings are persisted independently of the one in-flight request.
    return true;
}

const char* AgentVoicePhaseTitle(AgentVoicePhase phase) {
    switch (phase) {
        case AgentVoicePhase::kOffline:
            return "OFFLINE";
        case AgentVoicePhase::kConnecting:
            return "CONNECTING";
        case AgentVoicePhase::kOnline:
            return "ONLINE";
        case AgentVoicePhase::kSending:
            return "SENDING";
        case AgentVoicePhase::kThinking:
            return "THINKING";
        case AgentVoicePhase::kReceiving:
            return "RECEIVING";
        case AgentVoicePhase::kReadyToPlay:
            return "READY";
        case AgentVoicePhase::kPlaying:
            return "PLAYING";
        case AgentVoicePhase::kError:
            return "ERROR";
    }
    return "ERROR";
}
