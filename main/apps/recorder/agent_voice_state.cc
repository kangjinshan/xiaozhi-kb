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
        case AgentVoiceEvent::kReplyStarted:
            state->phase = AgentVoicePhase::kReceiving;
            return AgentVoiceAction::kReceiveReply;
        case AgentVoiceEvent::kReplyStored:
            state->phase = AgentVoicePhase::kReadyToPlay;
            return AgentVoiceAction::kPlayReply;
        case AgentVoiceEvent::kPlaybackStarted:
            state->phase = AgentVoicePhase::kPlaying;
            return AgentVoiceAction::kNone;
        case AgentVoiceEvent::kPlaybackFinished:
            state->phase = AgentVoicePhase::kOnline;
            state->queued_turn = false;
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

bool AgentVoiceCanRecord(const AgentVoiceState& state) {
    if (state.queued_turn) {
        return false;
    }
    return state.phase == AgentVoicePhase::kOffline ||
           state.phase == AgentVoicePhase::kOnline;
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
