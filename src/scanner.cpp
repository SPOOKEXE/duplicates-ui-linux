#include "scanner.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <unordered_map>

#include "groups.h"
#include "pipeline.h"
#include "util.h"

namespace {

std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir == "/") return "/" + name;
    return dir + "/" + name;
}

}  // namespace

std::vector<std::string> normalizeRoots(const std::vector<std::string>& roots) {
    std::vector<std::string> clean;
    for (const auto& raw : roots) {
        const std::string p = normalizePath(raw);
        if (p.empty()) continue;
        if (std::find(clean.begin(), clean.end(), p) == clean.end()) clean.push_back(p);
    }

    std::vector<std::string> out;
    for (const auto& p : clean) {
        bool nested = false;
        for (const auto& other : clean) {
            if (other != p && pathIsUnder(other, p)) {
                nested = true;
                break;
            }
        }
        if (!nested) out.push_back(p);
    }
    return out;
}

std::vector<FileEntry> walkRoots(const std::vector<std::string>& roots, const ScopeFilters& scope,
                                 const CompiledPipeline& pipe, const WalkHooks& hooks,
                                 WalkStats& stats) {
    std::vector<FileEntry> files;

    for (size_t r = 0; r < roots.size(); ++r) {
        // An explicit stack rather than recursion: a pathological tree cannot
        // blow the C stack, and cancellation is checked once per directory.
        std::vector<std::string> stack {roots[r]};

        while (!stack.empty()) {
            if (hooks.cancel && hooks.cancel->load()) return files;

            const std::string dir = stack.back();
            stack.pop_back();

            DIR* d = ::opendir(dir.c_str());
            if (!d) {
                ++stats.dirErrors;
                if (hooks.onError) hooks.onError(dir + ": " + std::strerror(errno));
                continue;
            }

            while (const dirent* e = ::readdir(d)) {
                const std::string name = e->d_name;
                if (name == "." || name == "..") continue;
                if (!scope.includeHidden && name[0] == '.') {
                    ++stats.skippedHidden;
                    continue;
                }

                const std::string path = joinPath(dir, name);
                struct stat st {};
                // lstat, not stat: a symlink must report as a symlink so it can
                // be skipped rather than counted as a copy of its target.
                if (::lstat(path.c_str(), &st) != 0) {
                    if (hooks.onError) hooks.onError(path + ": " + std::strerror(errno));
                    continue;
                }

                if (S_ISLNK(st.st_mode)) {
                    ++stats.skippedSymlink;
                    continue;
                }
                if (S_ISDIR(st.st_mode)) {
                    // An excluded directory is skipped whole rather than
                    // walked and filtered file by file, which is most of the
                    // reason to write an exclude rule in the first place.
                    if (dirPruned(path, pipe)) {
                        ++stats.prunedDirs;
                        if (hooks.onNote) hooks.onNote("pruned directory: " + path);
                        continue;
                    }
                    stack.push_back(path);
                    continue;
                }
                if (!S_ISREG(st.st_mode)) continue;  // fifos, sockets, devices

                const uint64_t size = static_cast<uint64_t>(st.st_size);
                if (size < pipe.minSize) {
                    ++stats.skippedSmall;
                    continue;
                }
                if (pipe.maxSize > 0 && size > pipe.maxSize) {
                    ++stats.skippedLarge;
                    continue;
                }
                if (!patternsAllow(path, pipe)) {
                    ++stats.skippedFiltered;
                    continue;
                }

                FileEntry f;
                f.path = path;
                f.size = size;
                f.dev = static_cast<uint64_t>(st.st_dev);
                f.ino = static_cast<uint64_t>(st.st_ino);
                f.nlink = static_cast<uint64_t>(st.st_nlink);
                f.mtime = static_cast<int64_t>(st.st_mtime);
                f.rootIndex = static_cast<int>(r);
                files.push_back(std::move(f));

                ++stats.filesSeen;
                stats.bytesSeen += size;
                if (hooks.onProgress && stats.filesSeen % 512 == 0) {
                    hooks.onProgress(stats.filesSeen, dir);
                }
            }
            ::closedir(d);
        }
    }

    if (hooks.onProgress) hooks.onProgress(stats.filesSeen, {});
    return files;
}

void collapseHardlinks(std::vector<FileEntry>& files, TieBreak tie, WalkStats& stats,
                       const WalkHooks* hooks) {
    // Only files with more than one link can possibly share an inode, and on a
    // normal tree that is a small minority, so the map stays small.
    struct Ident {
        uint64_t dev, ino;
        bool operator==(const Ident& o) const { return dev == o.dev && ino == o.ino; }
    };
    struct IdentHash {
        size_t operator()(const Ident& i) const {
            return static_cast<size_t>(i.dev * 1099511628211ULL ^ i.ino);
        }
    };

    std::unordered_map<Ident, size_t, IdentHash> firstSeen;
    std::vector<char> drop(files.size(), 0);

    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].nlink <= 1) continue;
        const Ident id {files[i].dev, files[i].ino};
        const auto it = firstSeen.find(id);
        if (it == firstSeen.end()) {
            firstSeen[id] = i;
            continue;
        }

        // Whichever of the two would win as a keeper becomes the representative;
        // the other is folded into it and dropped.
        size_t keep = it->second, fold = i;
        if (betterKeeper(files[fold], files[keep], tie)) std::swap(keep, fold);

        if (hooks && hooks->onNote) {
            hooks->onNote("folded hardlink: " + files[fold].path + " -> " + files[keep].path);
        }
        files[keep].alsoLinkedAt.push_back(files[fold].path);
        for (auto& extra : files[fold].alsoLinkedAt) {
            files[keep].alsoLinkedAt.push_back(std::move(extra));
        }
        drop[fold] = 1;
        firstSeen[id] = keep;
        ++stats.collapsedLinks;
    }

    size_t out = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        if (drop[i]) continue;
        if (out != i) files[out] = std::move(files[i]);
        ++out;
    }
    files.resize(out);
}
