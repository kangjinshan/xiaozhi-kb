#include "agent_turn_manifest.h"

#include <cstdint>
#include <cstdio>
#include <sys/stat.h>
#include <utility>

namespace {

constexpr size_t kMaxManifestBytes = 16 * 1024;
constexpr size_t kMaxPreviewBytes = 240;

bool ExtractString(const std::string& json,
                   const char* key,
                   std::string* output) {
    if (key == nullptr || output == nullptr) {
        return false;
    }
    const std::string marker = std::string("\"") + key + "\":\"";
    size_t position = json.find(marker);
    if (position == std::string::npos) {
        return false;
    }
    position += marker.size();

    std::string value;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char ch = json[position];
        if (escaped) {
            switch (ch) {
                case 'n': value.push_back('\n'); break;
                case 'r': value.push_back('\r'); break;
                case 't': value.push_back('\t'); break;
                case '\\': value.push_back('\\'); break;
                case '"': value.push_back('"'); break;
                default: return false;
            }
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            *output = std::move(value);
            return true;
        } else {
            value.push_back(ch);
        }
    }
    return false;
}

std::string NormalizePreview(const std::string& text) {
    std::string normalized;
    normalized.reserve(text.size());
    bool pending_space = false;
    for (unsigned char ch : text) {
        if (ch <= 0x20 || ch == 0x7F) {
            pending_space = !normalized.empty();
            continue;
        }
        if (pending_space) {
            normalized.push_back(' ');
            pending_space = false;
        }
        normalized.push_back(static_cast<char>(ch));
    }

    if (normalized.size() <= kMaxPreviewBytes) {
        return normalized;
    }
    size_t length = kMaxPreviewBytes;
    while (length > 0 &&
           (static_cast<unsigned char>(normalized[length]) & 0xC0) == 0x80) {
        --length;
    }
    normalized.resize(length);
    return normalized;
}

bool ReadBoundedFile(const std::string& path, std::string* output) {
    struct stat info = {};
    if (output == nullptr || stat(path.c_str(), &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<uint64_t>(info.st_size) > kMaxManifestBytes) {
        return false;
    }

    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    std::string data(static_cast<size_t>(info.st_size), '\0');
    const size_t read = std::fread(data.data(), 1, data.size(), file);
    const bool read_ok = read == data.size() && std::ferror(file) == 0;
    std::fclose(file);
    if (!read_ok) {
        return false;
    }
    *output = std::move(data);
    return true;
}

}  // namespace

bool AgentReadTurnConversation(const std::string& manifest_path,
                               AgentTurnConversation* conversation) {
    if (conversation == nullptr) {
        return false;
    }
    *conversation = {};

    std::string json;
    if (!ReadBoundedFile(manifest_path, &json)) {
        return false;
    }

    std::string transcript;
    std::string reply_text;
    const bool has_transcript = ExtractString(json, "transcript", &transcript);
    const bool has_reply = ExtractString(json, "reply_text", &reply_text);
    if (!has_transcript && !has_reply) {
        return false;
    }
    if (has_transcript) {
        conversation->transcript = NormalizePreview(transcript);
    }
    if (has_reply) {
        conversation->reply_text = NormalizePreview(reply_text);
    }
    return true;
}
