#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dupes.h"

class HashCache;

// Where the scan is. Reported so the progress line can say what is actually
// happening rather than showing one undifferentiated bar.
enum class Stage {
    Idle,
    Walking,
    Sizing,
    HeadBytes,
    FullHash,
    ExactCompare,
    Grouping,
    Done,
    Cancelled,
};

const char* stageName(Stage s);

struct CascadeProgress {
    Stage stage = Stage::Idle;
    uint64_t done = 0;
    uint64_t total = 0;
    uint64_t candidates = 0;  // files still in the running after the last stage
    uint64_t bytesRead = 0;
};

struct CascadeHooks {
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const CascadeProgress&)> onProgress;
    std::function<void(const std::string&)> onError;
    HashCache* cache = nullptr;
    int threads = 4;
};

// Turns a walked, hardlink-collapsed file list into duplicate groups.
//
// Every stage only ever sees what survived the previous one, and any bucket
// that falls to a single file is dropped immediately, so each stage reads
// strictly less than the one before it. Hashes are written back into `files`.
//
// Keeper and selection are not decided here; that is resolveKeepers' job.
std::vector<DupGroup> runCascade(std::vector<FileEntry>& files, const StageSettings& st,
                                 const CascadeHooks& hooks);
