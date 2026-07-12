#ifndef RECORDER_FILE_LIST_H_
#define RECORDER_FILE_LIST_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct RecorderFileEntry {
    std::string name;
    std::string path;
    std::string date;
    std::string turn_id;
    std::string conversation_text;
    int index = -1;
    uint32_t size_bytes = 0;
};

bool RecorderParseRecordingFilename(const char* filename, int* index);
std::vector<RecorderFileEntry> RecorderListRecordings(const char* dir, size_t max_entries);
std::vector<RecorderFileEntry> RecorderListAgentRecordings(const char* root,
                                                           size_t max_entries);
std::string RecorderFormatRecordingDetail(const RecorderFileEntry& entry);
std::string RecorderConversationLabel(const RecorderFileEntry& entry);

#endif  // RECORDER_FILE_LIST_H_
