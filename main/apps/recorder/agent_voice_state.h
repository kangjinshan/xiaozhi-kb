#ifndef AGENT_VOICE_STATE_H_
#define AGENT_VOICE_STATE_H_

enum class AgentVoicePhase {
    kOffline,
    kConnecting,
    kOnline,
    kSending,
    kThinking,
    kReceiving,
    kReadyToPlay,
    kPlaying,
    kError,
};

enum class AgentVoiceEvent {
    kWifiConnected,
    kServerReady,
    kTurnQueued,
    kTurnAccepted,
    kReplyStarted,
    kReplyStored,
    kPlaybackStarted,
    kPlaybackFinished,
    kDisconnected,
    kFailure,
};

enum class AgentVoiceAction {
    kNone,
    kConnectSocket,
    kSendQueuedTurn,
    kWaitForReply,
    kReceiveReply,
    kPlayReply,
    kScheduleReconnect,
};

struct AgentVoiceState {
    AgentVoicePhase phase = AgentVoicePhase::kOffline;
    bool queued_turn = false;
};

AgentVoiceAction AgentVoiceReduce(AgentVoiceState* state, AgentVoiceEvent event);
bool AgentVoiceCanRecord(const AgentVoiceState& state);
const char* AgentVoicePhaseTitle(AgentVoicePhase phase);

#endif  // AGENT_VOICE_STATE_H_
