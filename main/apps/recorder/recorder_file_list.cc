#include "recorder_file_list.h"

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
            for (const char* name : {"assistant.wav", "user.wav"}) {
                RecorderFileEntry entry;
                entry.name = name;
                entry.path = turn_path + "/" + name;
                entry.date = date;
                entry.turn_id = turn_id;
                if (IsRegularFile(entry.path, &entry.size_bytes)) {
                    entries.push_back(std::move(entry));
                }
            }
        }
        closedir(date_directory);
    }
    closedir(root_directory);

    std::sort(entries.begin(), entries.end(),
              [](const RecorderFileEntry& a, const RecorderFileEntry& b) {
                  if (a.date != b.date) {
                      return a.date > b.date;
                  }
                  if (a.turn_id != b.turn_id) {
                      return a.turn_id > b.turn_id;
                  }
                  return a.name < b.name;
              });
    if (entries.size() > max_entries) {
        entries.resize(max_entries);
    }
    return entries;
}

std::string RecorderFormatRecordingDetail(const RecorderFileEntry& entry) {
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
