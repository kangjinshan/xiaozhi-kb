#include "recorder_file_list.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

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
