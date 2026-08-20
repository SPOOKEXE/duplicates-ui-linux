#include "runs.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "log.h"
#include "util.h"

namespace fs = std::filesystem;

namespace {

constexpr const char* kHeader = "duplicates-ui run v1";

// The manifests that make a quarantine reversible sit next to the files they
// describe, in a directory the scan itself would skip.
constexpr const char* kManifestDirName = ".duplicates-ui";

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

bool exists(const std::string& path) {
    struct stat st {};
    return ::lstat(path.c_str(), &st) == 0;
}

RunEntry parseManifest(const std::string& path) {
    RunEntry e;
    e.manifest = path;

    std::ifstream in(path);
    if (!in) return e;
    std::string line;
    if (!std::getline(in, line) || line != kHeader) {
        e.manifest.clear();  // not ours
        return e;
    }

    while (std::getline(in, line)) {
        const std::vector<std::string> f = splitTabs(line);
        if (f[0] == "kind" && f.size() >= 2) {
            e.kind = f[1] == "delete" ? ActionKind::Delete : ActionKind::Quarantine;
        } else if (f[0] == "root" && f.size() >= 2) {
            e.root = unescapeField(f[1]);
        } else if (f[0] == "stamp" && f.size() >= 2) {
            e.stamp = f[1];
        } else if (f[0] == "file" && f.size() >= 6) {
            ++e.files;
            e.bytes += std::strtoull(f[3].c_str(), nullptr, 10);
        } else if (f[0] == "restored") {
            e.restored = true;
        }
    }
    return e;
}

void collectFrom(const std::string& dir, std::vector<RunEntry>& out) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return;
    for (const auto& entry : it) {
        const std::string p = entry.path().string();
        if (p.size() < 4 || p.compare(p.size() - 4, 4, ".tsv") != 0) continue;
        RunEntry e = parseManifest(p);
        if (!e.manifest.empty()) out.push_back(std::move(e));
    }
}

}  // namespace

const char* actionKindName(ActionKind k) {
    return k == ActionKind::Delete ? "delete" : "quarantine";
}

std::string deleteRunsDir() { return stateDir() + "/runs"; }

std::string quarantineRunsDir(const std::string& quarantineRoot) {
    return quarantineRoot + "/" + kManifestDirName;
}

std::string newManifestPath(ActionKind kind, const std::string& quarantineRoot,
                            const std::string& stamp) {
    const std::string dir = kind == ActionKind::Delete ? deleteRunsDir()
                                                       : quarantineRunsDir(quarantineRoot);
    return dir + "/run-" + stamp + ".tsv";
}

ManifestWriter::~ManifestWriter() { close(); }

bool ManifestWriter::open(const std::string& path, ActionKind kind, const std::string& root) {
    close();
    if (!ensureDir(fs::path(path).parent_path().string())) return false;

    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return false;

    std::fprintf(f, "%s\n", kHeader);
    std::fprintf(f, "kind\t%s\n", actionKindName(kind));
    std::fprintf(f, "root\t%s\n", escapeField(root).c_str());
    std::fprintf(f, "stamp\t%s\n", timestampNow().c_str());
    std::fflush(f);

    file_ = f;
    path_ = path;
    return true;
}

bool ManifestWriter::append(const std::string& original, const std::string& dest, uint64_t size,
                            int64_t mtime, uint64_t hash) {
    if (!file_) return false;
    FILE* f = static_cast<FILE*>(file_);
    std::fprintf(f, "file\t%s\t%s\t%" PRIu64 "\t%" PRId64 "\t%" PRIu64 "\n",
                 escapeField(original).c_str(), escapeField(dest).c_str(), size, mtime, hash);
    // Flushed here, before the caller removes anything: the record has to reach
    // the disk first so a crash cannot orphan a file with no way back.
    return std::fflush(f) == 0;
}

void ManifestWriter::markRestored() {
    if (!file_) return;
    std::fprintf(static_cast<FILE*>(file_), "restored\t1\n");
    std::fflush(static_cast<FILE*>(file_));
}

void ManifestWriter::close() {
    if (!file_) return;
    std::fclose(static_cast<FILE*>(file_));
    file_ = nullptr;
}

std::vector<RunEntry> loadRuns(const std::string& quarantineRoot) {
    std::vector<RunEntry> out;
    collectFrom(deleteRunsDir(), out);
    if (!quarantineRoot.empty()) collectFrom(quarantineRunsDir(quarantineRoot), out);

    // Newest first. The stamp sorts correctly as text, which is the point of the
    // format it is written in.
    std::sort(out.begin(), out.end(),
              [](const RunEntry& a, const RunEntry& b) { return a.stamp > b.stamp; });
    return out;
}

RestoreResult restoreRun(const RunEntry& run, Log* log) {
    RestoreResult r;
    const auto note = [&](LogLevel level, const std::string& m) {
        if (log) log->add(level, m);
    };
    note(LogLevel::Warn, "restore started from " + run.manifest);

    if (run.kind == ActionKind::Delete) {
        r.notes.push_back("a delete run cannot be restored");
        return r;
    }

    std::ifstream in(run.manifest);
    if (!in) {
        r.notes.push_back("cannot read " + run.manifest);
        return r;
    }

    std::string line;
    std::getline(in, line);  // header, already validated by loadRuns

    while (std::getline(in, line)) {
        const std::vector<std::string> f = splitTabs(line);
        if (f[0] != "file" || f.size() < 6) continue;

        const std::string original = unescapeField(f[1]);
        const std::string dest = unescapeField(f[2]);

        if (!exists(dest)) {
            ++r.skipped;
            r.notes.push_back("gone from quarantine: " + dest);
            note(LogLevel::Warn, "skipped, gone from quarantine: " + dest);
            continue;
        }
        if (exists(original)) {
            // Something lives there again. Overwriting it would be exactly the
            // data loss this whole feature exists to avoid.
            ++r.skipped;
            r.notes.push_back("path is occupied again: " + original);
            note(LogLevel::Warn, "skipped, the path is occupied again: " + original);
            continue;
        }
        if (!ensureDir(fs::path(original).parent_path().string())) {
            ++r.failed;
            r.notes.push_back("cannot recreate directory for " + original);
            note(LogLevel::Error, "cannot recreate the directory for " + original);
            continue;
        }

        std::string err;
        if (moveFile(dest, original, err)) {
            ++r.restored;
            note(LogLevel::Info, "restored: " + dest + " -> " + original);
        } else {
            ++r.failed;
            r.notes.push_back(original + ": " + err);
            note(LogLevel::Error, "restore failed: " + original + " (" + err + ")");
        }
    }

    if (r.restored > 0 && r.failed == 0) {
        // Reopening in append mode keeps the record of what the run did; only
        // the footer is added.
        if (FILE* f = std::fopen(run.manifest.c_str(), "a")) {
            std::fprintf(f, "restored\t1\n");
            std::fclose(f);
        }
    }

    note(LogLevel::Warn, "restore finished: " + formatCount(r.restored) + " restored, " +
                             formatCount(r.skipped) + " skipped, " + formatCount(r.failed) +
                             " failed");
    return r;
}
