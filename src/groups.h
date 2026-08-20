#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "dupes.h"

// Which of two files should survive. The input directory's position in the
// priority list decides it; the tie-break rule settles members that rank
// equally, which includes every copy inside a single input directory.
bool betterKeeper(const FileEntry& a, const FileEntry& b, TieBreak tie);

// Recomputes the keeper of every group that has not been pinned by hand, and
// repairs the selection so exactly the non-keepers stay ticked.
void resolveKeepers(std::vector<DupGroup>& groups, const std::vector<FileEntry>& files,
                    TieBreak tie);

// Pins the group: priority changes will no longer move this keeper.
void setKeeper(DupGroup& g, int memberIndex);

void selectAllExtras(std::vector<DupGroup>& groups);
void deselectAll(std::vector<DupGroup>& groups);
void invertSelection(std::vector<DupGroup>& groups);
void clearPins(std::vector<DupGroup>& groups);

struct Totals {
    uint64_t groups = 0;
    uint64_t extras = 0;       // members that are not keepers
    uint64_t reclaimable = 0;  // bytes freed if every extra went
    uint64_t selected = 0;
    uint64_t selectedBytes = 0;
};

Totals computeTotals(const std::vector<DupGroup>& groups, const std::vector<FileEntry>& files);

// The results view. Purely a filter over what is drawn; nothing is discarded.
struct GroupFilter {
    std::string text;      // matched case-insensitively against member paths
    uint64_t minSize = 0;  // per-file size, not per-group
    int minMembers = 2;
};

bool groupMatches(const DupGroup& g, const std::vector<FileEntry>& files, const GroupFilter& f);

enum class GroupSort { ReclaimableDesc, SizeDesc, MembersDesc, PathAsc };
extern const char* const kGroupSortNames[4];

void sortGroups(std::vector<DupGroup>& groups, const std::vector<FileEntry>& files, GroupSort s);

// Drops members whose file is gone from disk, then drops any group left with
// fewer than two members. Called after an apply run so the list reflects
// reality without needing a rescan.
void pruneRemoved(std::vector<DupGroup>& groups, const std::vector<FileEntry>& files,
                  const std::unordered_set<std::string>& removed);
