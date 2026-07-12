#include "recorder_file_list.h"

#include "agent_turn_manifest.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

bool IsDirectory(const std::string& path) {
    struct stat info = {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool IsRegularFile(const std::string& path, uint32_t* size_bytes) {
    struct stat info = {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0) {
        return false;
    }
    if (size_bytes != nullptr) {
        *size_bytes = static_cast<uint32_t>(info.st_size);
    }
    return true;
}

struct AgentHistoryIndexEntry {
    std::string date;
    std::string turn_id;
};

bool ExtractIndexString(const std::string& json,
                        const char* key,
                        std::string* output) {
    if (key == nullptr || output == nullptr) {
        return false;
    }
    const std::string marker = std::string("\"") + key + "\":\"";
    const size_t start = json.find(marker);
    if (start == std::string::npos) {
        return false;
    }
    const size_t value_start = start + marker.size();
    const size_t value_end = json.find('"', value_start);
    if (value_end == std::string::npos || value_end == value_start) {
        return false;
    }
    *output = json.substr(value_start, value_end - value_start);
    return true;
}

std::vector<AgentHistoryIndexEntry> ReadRecentHistoryIndex(
        const std::string& root) {
    constexpr size_t kMaxRecentTurns = 128;
    constexpr size_t kMaxIndexLineBytes = 512;
    std::vector<AgentHistoryIndexEntry> index;
    FILE* file = std::fopen((root + "/turns.jsonl").c_str(), "rb");
    if (file == nullptr) {
        return index;
    }

    char line[kMaxIndexLineBytes];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        const size_t length = std::strlen(line);
        if (length == 0) {
            continue;
        }
        if (line[length - 1] != '\n' && !std::feof(file)) {
            int ch = 0;
            while ((ch = std::fgetc(file)) != '\n' && ch != EOF) {
            }
            continue;
        }

        AgentHistoryIndexEntry entry;
        const std::string json(line, length);
        if (!ExtractIndexString(json, "date", &entry.date) ||
            !ExtractIndexString(json, "turn_id", &entry.turn_id)) {
            continue;
        }
        if (index.size() == kMaxRecentTurns) {
            index.erase(index.begin());
        }
        index.push_back(std::move(entry));
    }
    std::fclose(file);
    return index;
}

size_t FindHistoryOrder(const std::vector<AgentHistoryIndexEntry>& index,
                        const RecorderFileEntry& entry) {
    for (size_t position = index.size(); position > 0; --position) {
        const auto& candidate = index[position - 1];
        if (candidate.date == entry.date && candidate.turn_id == entry.turn_id) {
            return position;
        }
    }
    return 0;
}

bool IsCalendarDate(const std::string& date) {
    return date.size() == 8 &&
        std::all_of(date.begin(), date.end(), [](unsigned char ch) {
            return ch >= '0' && ch <= '9';
        });
}

void LimitWithoutSplittingTurn(std::vector<RecorderFileEntry>* entries,
                               size_t max_entries) {
    if (entries == nullptr || entries->size() <= max_entries) {
        return;
    }
    size_t limit = max_entries;
    if (limit > 0 && limit < entries->size() &&
        !(*entries)[limit - 1].turn_id.empty() &&
        (*entries)[limit - 1].date == (*entries)[limit].date &&
        (*entries)[limit - 1].turn_id == (*entries)[limit].turn_id) {
        --limit;
    }
    entries->resize(limit);
}

}  // namespace

bool RecorderParseRecordingFilename(const char* filename, int* index) {
    if (filename == nullptr) {
        return false;
    }

    int parsed_index = -1;
    int consumed = 0;
    if (std::sscanf(filename, "rec%d.wav%n", &parsed_index, &consumed) != 1 ||
        consumed <= 0 || filename[consumed] != '\0' || parsed_index < 0) {
        return false;
    }

    if (index != nullptr) {
        *index = parsed_index;
    }
    return true;
}

std::vector<RecorderFileEntry> RecorderListRecordings(const char* dir, size_t max_entries) {
    std::vector<RecorderFileEntry> entries;
    if (dir == nullptr || max_entries == 0) {
        return entries;
    }

    DIR* handle = opendir(dir);
    if (handle == nullptr) {
        return entries;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(handle)) != nullptr) {
        int index = -1;
        if (!RecorderParseRecordingFilename(ent->d_name, &index)) {
            continue;
        }

        RecorderFileEntry entry;
        entry.name = ent->d_name;
        entry.path = std::string(dir) + "/" + ent->d_name;
        entry.index = index;

        struct stat st = {};
        if (stat(entry.path.c_str(), &st) == 0 && st.st_size > 0) {
            entry.size_bytes = static_cast<uint32_t>(st.st_size);
        }

        entries.push_back(entry);
    }
    closedir(handle);

    std::sort(entries.begin(), entries.end(),
              [](const RecorderFileEntry& a, const RecorderFileEntry& b) {
                  if (a.index != b.index) {
                      return a.index > b.index;
                  }
                  return a.name > b.name;
              });

    if (entries.size() > max_entries) {
        entries.resize(max_entries);
    }
    return entries;
}

std::vector<RecorderFileEntry> RecorderListAgentRecordings(const char* root,
                                                           size_t max_entries) {
    std::vector<RecorderFileEntry> entries;
    if (root == nullptr || max_entries == 0) {
        return entries;
    }
    DIR* root_directory = opendir(root);
    if (root_directory == nullptr) {
        return entries;
    }
    struct dirent* date_entry = nullptr;
    while ((date_entry = readdir(root_directory)) != nullptr) {
        const std::string date = date_entry->d_name;
        const std::string date_path = std::string(root) + "/" + date;
        if (date == "." || date == ".." || !IsDirectory(date_path)) {
            continue;
        }
        DIR* date_directory = opendir(date_path.c_str());
        if (date_directory == nullptr) {
            continue;
        }
        struct dirent* turn_entry = nullptr;
        while ((turn_entry = readdir(date_directory)) != nullptr) {
            const std::string turn_id = turn_entry->d_name;
            const std::string turn_path = date_path + "/" + turn_id;
            if (turn_id == "." || turn_id == ".." || !IsDirectory(turn_path)) {
                continue;
            }
            AgentTurnConversation conversation;
            AgentReadTurnConversation(turn_path + "/turn.json", &conversation);
            for (const char* name : {"assistant.wav", "user.wav"}) {
                RecorderFileEntry entry;
                entry.name = name;
                entry.path = turn_path + "/" + name;
                entry.date = date;
                entry.turn_id = turn_id;
                entry.conversation_text = entry.name == "assistant.wav"
                    ? conversation.reply_text
                    : conversation.transcript;
                if (IsRegularFile(entry.path, &entry.size_bytes)) {
                    entries.push_back(std::move(entry));
                }
            }
        }
        closedir(date_directory);
    }
    closedir(root_directory);

    const auto history_index = ReadRecentHistoryIndex(root);
    std::sort(entries.begin(), entries.end(),
              [&history_index](const RecorderFileEntry& a,
                               const RecorderFileEntry& b) {
                  const bool a_has_date = IsCalendarDate(a.date);
                  const bool b_has_date = IsCalendarDate(b.date);
                  if (a_has_date != b_has_date) {
                      return a_has_date;
                  }
                  if (a.date != b.date) {
                      return a.date > b.date;
                  }
                  const size_t a_order = FindHistoryOrder(history_index, a);
                  const size_t b_order = FindHistoryOrder(history_index, b);
                  if (a_order != b_order && a_order != 0 && b_order != 0) {
                      return a_order > b_order;
                  }
                  if ((a_order != 0) != (b_order != 0)) {
                      return a_order != 0;
                  }
                  if (a.turn_id != b.turn_id) {
                      return a.turn_id > b.turn_id;
                  }
                  return a.name < b.name;
              });
    LimitWithoutSplittingTurn(&entries, max_entries);
    return entries;
}

std::string RecorderFormatRecordingDetail(const RecorderFileEntry& entry) {
    if (!entry.conversation_text.empty()) {
        return entry.conversation_text;
    }
    char detail[32];
    if (entry.size_bytes >= 1024 * 1024) {
        const unsigned mb10 = (entry.size_bytes * 10U) / (1024U * 1024U);
        std::snprintf(detail, sizeof(detail), "%u.%u MB", mb10 / 10, mb10 % 10);
    } else if (entry.size_bytes >= 1024) {
        const unsigned kb = (entry.size_bytes + 1023U) / 1024U;
        std::snprintf(detail, sizeof(detail), "%u KB", kb);
    } else {
        std::snprintf(detail, sizeof(detail), "%u B", static_cast<unsigned>(entry.size_bytes));
    }
    return detail;
}

std::string RecorderConversationLabel(const RecorderFileEntry& entry) {
    if (!entry.turn_id.empty() && entry.name == "assistant.wav") {
        return "AI 回复";
    }
    if (!entry.turn_id.empty() && entry.name == "user.wav") {
        return "你";
    }
    return "录音";
}
