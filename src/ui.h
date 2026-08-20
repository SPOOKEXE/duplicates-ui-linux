#pragma once

#include <imgui.h>

#include <string>
#include <vector>

#include "actions.h"
#include "dir_browser.h"
#include "dupes.h"
#include "groups.h"
#include "hash_cache.h"
#include "scan_engine.h"
#include "session.h"

// Which list the folder picker is currently filling in.
enum class BrowserTarget { None, InputRoot, Quarantine };

// One drawn line in the results view. member is -1 for a group's own header
// row. Flattening groups into rows up front is what lets the list be clipped:
// only the rows actually on screen are ever drawn.
struct ResultRow {
    int group = 0;
    int member = -1;
};

struct AppState {
    // Inputs. Order is priority: the first directory wins.
    std::vector<std::string> roots;
    ScopeFilters scope;
    StageSettings stages;
    TieBreak tie = TieBreak::OldestMtime;

    HashCache cache;
    ScanEngine engine;
    // The roots the running scan actually used: overlapping entries are folded
    // out first, and a file's rootIndex points into this list, not into roots.
    std::vector<std::string> scanRoots;

    // Results, owned here rather than in the engine so no frame ever copies
    // them. The engine hands them over exactly once, when a scan finishes.
    std::vector<FileEntry> files;
    std::vector<DupGroup> groups;
    ScanStats stats;
    bool haveResults = false;

    // Recomputed once a frame in drawUi, so the top bar, the results header and
    // the action bar all read the same numbers without walking the groups three
    // times.
    Totals totals;

    GroupFilter filter;
    GroupSort sort = GroupSort::ReclaimableDesc;
    std::vector<char> expanded;  // one per group
    std::vector<ResultRow> rows;
    bool rowsDirty = true;

    ActionKind action = ActionKind::Quarantine;
    std::string quarantineRoot;
    ActionQueue actions;

    std::vector<RunEntry> runs;
    bool runsDirty = true;
    RunSummary lastRun;
    bool haveLastRun = false;
    RestoreResult lastRestore;
    bool haveLastRestore = false;

    // UI-only scratch state.
    DirBrowser browser;
    BrowserTarget browserTarget = BrowserTarget::None;
    char rootBuf[1024] = {};
    char globBuf[512] = {};
    char filterBuf[256] = {};
    char quarBuf[1024] = {};
    bool askApplyConfirm = false;
    int askRestoreIndex = -1;
    bool showLog = true;
    std::vector<std::string> droppedPaths;
    std::string notice;  // one-line message under the top bar, dismissed by the user
};

extern const ImVec4 kDim;
extern const ImVec4 kOk;
extern const ImVec4 kBad;
extern const ImVec4 kWarn;
extern const ImVec4 kCyan;

void applyTheme();

SessionData sessionFromState(const AppState& s);
void applySession(AppState& s, const SessionData& d);

// Turns this frame's dropped paths into input directories.
void handleDrops(AppState& s);

void drawUi(AppState& s);

// Split across ui_inputs.cpp, ui_results.cpp and ui_runs.cpp so no single file
// carries the whole interface.
void drawInputs(AppState& s);
void drawScopeAndStages(AppState& s);
// reserveBottom is the room to leave for the action bar underneath.
void drawResults(AppState& s, float reserveBottom);
void drawActionBar(AppState& s);
void drawRunsTab(AppState& s);
void drawLogTab(AppState& s);

// Recomputes the flat row list from the groups, the filter and what is expanded.
void rebuildRows(AppState& s);

// Everything currently ticked, paired with the copy that will survive it.
std::vector<ActionItem> collectSelected(const AppState& s);

// Starts a scan, or reports why it cannot.
void startScan(AppState& s);
