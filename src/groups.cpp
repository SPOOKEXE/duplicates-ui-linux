#include "groups.h"

#include <algorithm>
#include <cctype>

namespace {

int segmentCount(const std::string& path) {
    return static_cast<int>(std::count(path.begin(), path.end(), '/'));
}

std::string lowered(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

}  // namespace

bool betterKeeper(const FileEntry& a, const FileEntry& b, TieBreak tie) {
    if (a.rootIndex != b.rootIndex) return a.rootIndex < b.rootIndex;

    switch (tie) {
        case TieBreak::OldestMtime:
            if (a.mtime != b.mtime) return a.mtime < b.mtime;
            break;
        case TieBreak::NewestMtime:
            if (a.mtime != b.mtime) return a.mtime > b.mtime;
            break;
        case TieBreak::ShortestPath:
            if (a.path.size() != b.path.size()) return a.path.size() < b.path.size();
            break;
        case TieBreak::FewestSegments: {
            const int sa = segmentCount(a.path), sb = segmentCount(b.path);
            if (sa != sb) return sa < sb;
            break;
        }
        case TieBreak::Alphabetical:
            break;
    }
    // Falling through to the path keeps the answer stable: the same scan must
    // pick the same keeper every time, whatever the rule left undecided.
    return a.path < b.path;
}

void resolveKeepers(std::vector<DupGroup>& groups, const std::vector<FileEntry>& files,
                    TieBreak tie) {
    for (auto& g : groups) {
        if (g.members.empty()) continue;
        if (g.keeper < 0 || g.keeper >= static_cast<int>(g.members.size())) g.keeper = 0;

        if (!g.userPinned) {
            int best = 0;
            for (size_t i = 1; i < g.members.size(); ++i) {
                if (betterKeeper(files[g.members[i].fileIndex], files[g.members[best].fileIndex],
                                 tie)) {
                    best = static_cast<int>(i);
                }
            }
            if (best != g.keeper) {
                // The file that just lost the job becomes actionable, so the
                // number of protected copies stays at exactly one.
                g.members[g.keeper].selected = true;
                g.keeper = best;
            }
        }
        g.members[g.keeper].selected = false;
    }
}

void setKeeper(DupGroup& g, int memberIndex) {
    if (memberIndex < 0 || memberIndex >= static_cast<int>(g.members.size())) return;
    if (memberIndex != g.keeper) {
        g.members[g.keeper].selected = true;
        g.keeper = memberIndex;
    }
    g.members[g.keeper].selected = false;
    g.userPinned = true;
}

void selectAllExtras(std::vector<DupGroup>& groups) {
    for (auto& g : groups) {
        for (size_t i = 0; i < g.members.size(); ++i) {
            g.members[i].selected = (static_cast<int>(i) != g.keeper);
        }
    }
}

void deselectAll(std::vector<DupGroup>& groups) {
    for (auto& g : groups) {
        for (auto& m : g.members) m.selected = false;
    }
}

void invertSelection(std::vector<DupGroup>& groups) {
    for (auto& g : groups) {
        for (size_t i = 0; i < g.members.size(); ++i) {
            if (static_cast<int>(i) == g.keeper) continue;
            g.members[i].selected = !g.members[i].selected;
        }
    }
}

void clearPins(std::vector<DupGroup>& groups) {
    for (auto& g : groups) g.userPinned = false;
}

Totals computeTotals(const std::vector<DupGroup>& groups, const std::vector<FileEntry>& files) {
    Totals t;
    t.groups = groups.size();
    for (const auto& g : groups) {
        t.extras += g.members.size() - 1;
        t.reclaimable += (g.members.size() - 1) * g.size;
        for (size_t i = 0; i < g.members.size(); ++i) {
            if (!g.members[i].selected) continue;
            ++t.selected;
            t.selectedBytes += files[g.members[i].fileIndex].size;
        }
    }
    return t;
}

bool groupMatches(const DupGroup& g, const std::vector<FileEntry>& files, const GroupFilter& f) {
    if (g.size < f.minSize) return false;
    if (static_cast<int>(g.members.size()) < f.minMembers) return false;
    if (f.text.empty()) return true;

    const std::string needle = lowered(f.text);
    for (const auto& m : g.members) {
        if (lowered(files[m.fileIndex].path).find(needle) != std::string::npos) return true;
    }
    return false;
}

const char* const kGroupSortNames[4] = {"reclaimable", "size", "copies", "path"};

void sortGroups(std::vector<DupGroup>& groups, const std::vector<FileEntry>& files, GroupSort s) {
    const auto reclaim = [](const DupGroup& g) { return (g.members.size() - 1) * g.size; };
    const auto keeperPath = [&files](const DupGroup& g) -> const std::string& {
        return files[g.members[g.keeper].fileIndex].path;
    };

    std::stable_sort(groups.begin(), groups.end(), [&](const DupGroup& a, const DupGroup& b) {
        switch (s) {
            case GroupSort::ReclaimableDesc: {
                const uint64_t ra = reclaim(a), rb = reclaim(b);
                if (ra != rb) return ra > rb;
                break;
            }
            case GroupSort::SizeDesc:
                if (a.size != b.size) return a.size > b.size;
                break;
            case GroupSort::MembersDesc:
                if (a.members.size() != b.members.size()) return a.members.size() > b.members.size();
                break;
            case GroupSort::PathAsc:
                break;
        }
        return keeperPath(a) < keeperPath(b);
    });
}

void pruneRemoved(std::vector<DupGroup>& groups, const std::vector<FileEntry>& files,
                  const std::unordered_set<std::string>& removed) {
    if (removed.empty()) return;

    std::vector<DupGroup> kept;
    kept.reserve(groups.size());

    for (auto& g : groups) {
        const std::string keeperPath = files[g.members[g.keeper].fileIndex].path;

        std::vector<Member> members;
        for (const auto& m : g.members) {
            if (removed.count(files[m.fileIndex].path) == 0) members.push_back(m);
        }
        if (members.size() < 2) continue;  // nothing left to decide

        g.members = std::move(members);
        g.keeper = 0;
        for (size_t i = 0; i < g.members.size(); ++i) {
            if (files[g.members[i].fileIndex].path == keeperPath) {
                g.keeper = static_cast<int>(i);
                break;
            }
        }
        g.members[g.keeper].selected = false;
        kept.push_back(std::move(g));
    }
    groups = std::move(kept);
}
