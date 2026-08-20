#pragma once

#include <string>
#include <vector>

#include "dupes.h"
#include "groups.h"
#include "runs.h"

// Everything worth surviving a restart. Scan results are deliberately absent:
// they are large, they go stale the moment anything on disk changes, and a
// rescan with a warm hash cache is fast.
struct SessionData {
    std::vector<std::string> roots;
    ScopeFilters scope;
    StageSettings stages;
    TieBreak tie = TieBreak::OldestMtime;
    std::string quarantineRoot;
    ActionKind action = ActionKind::Quarantine;
    GroupSort sort = GroupSort::ReclaimableDesc;
    GroupFilter filter;
    bool showLog = true;
};

// $XDG_STATE_HOME/duplicates-ui/session.tsv, falling back to ~/.local/state.
std::string sessionPath();

// Tab separated, one record per line, with backslash, tab and newline escaped.
// Deliberately not JSON: paths are the only tricky field, and escaping three
// characters is less code than pulling in a parser.
std::string serializeSession(const SessionData& d);
SessionData parseSession(const std::string& text);

// Writes only when the text differs from what was last written, so an idle app
// does not churn the disk. lastHash is the caller's memory of the last write.
bool saveSessionIfChanged(const std::string& path, const std::string& text, size_t& lastHash);

std::string readFileOrEmpty(const std::string& path);
