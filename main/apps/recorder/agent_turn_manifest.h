#ifndef AGENT_TURN_MANIFEST_H_
#define AGENT_TURN_MANIFEST_H_

#include <string>

struct AgentTurnConversation {
    std::string transcript;
    std::string reply_text;
};

// Reads only the published, bounded turn manifest. Text is normalized to a
// single-line UTF-8 preview suitable for the history list.
bool AgentReadTurnConversation(const std::string& manifest_path,
                               AgentTurnConversation* conversation);

#endif  // AGENT_TURN_MANIFEST_H_
