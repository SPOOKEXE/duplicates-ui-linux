#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dupes.h"
#include "pipeline.h"

class HashCache;

// Where the scan is. Reported so the progress line can say what is actually
// happening rather than showing one undifferentiated bar.
enum class Stage {
    Idle,
    Walking,
    Sizing,
    SameName,
    SameMtime,
    HeadBytes,
    FullHash,
    ExactCompare,
    Grouping,
    Done,
    Cancelled,
};

const char* stageName(Stage s);

// Which stage a split row runs as. Drop rows never reach the cascade.
Stage stageForRule(RuleKind kind);

struct CascadeProgress {
    Stage stage = Stage::Idle;
    uint64_t done = 0;
    uint64_t total = 0;
    uint64_t candidates = 0;  // files still in the running after the last row
    uint64_t bytesRead = 0;
};

struct CascadeHooks {
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(const CascadeProgress&)> onProgress;
    std::function<void(const std::string&)> onError;
    // One line per row as it finishes, saying what went in and what came out.
    std::function<void(const std::string&)> onNote;
    HashCache* cache = nullptr;
};

// Turns a walked, hardlink-collapsed file list into duplicate groups by running
// the pipeline's split rows in the order the user put them in.
//
// Size always goes first and is not a row: it comes free from the walk's lstat,
// and it is what keeps every later row affordable. Without it, an exact byte
// compare placed at the top would read every file against every other file.
//
// Every row only ever sees what survived the previous one, and any bucket that
// falls to a single file is dropped immediately. Hashes are written back into
// `files`.
//
// Keeper and selection are not decided here; that is resolveKeepers' job.
std::vector<DupGroup> runCascade(std::vector<FileEntry>& files, const CompiledPipeline& pipe,
                                 const CascadeHooks& hooks);
