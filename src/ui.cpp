#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include "scanner.h"
#include "util.h"

const ImVec4 kDim(0.55f, 0.55f, 0.55f, 1.0f);
const ImVec4 kOk(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kBad(1.00f, 0.35f, 0.35f, 1.0f);
const ImVec4 kWarn(1.00f, 0.75f, 0.30f, 1.0f);
const ImVec4 kCyan(0.55f, 0.75f, 1.00f, 1.0f);

namespace {

void drawTopBar(AppState& s) {
    const bool scanning = s.engine.running();
    const bool applying = s.actions.running();

    ImGui::BeginDisabled(scanning || applying || s.roots.empty());
    if (ImGui::Button("Scan", ImVec2(90, 0))) startScan(s);
    ImGui::EndDisabled();
    if (s.roots.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("add at least one input directory first");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!scanning);
    if (ImGui::Button("Cancel", ImVec2(90, 0))) s.engine.cancel();
    ImGui::EndDisabled();

    ImGui::SameLine();
    const CascadeProgress p = s.engine.progress();
    if (scanning) {
        ImGui::TextColored(kCyan, "%s", stageName(p.stage));
        ImGui::SameLine();
        // Bytes when the row reads them: a file count sits still for an hour on
        // one huge archive and reads as a hang. Files otherwise.
        if (p.stageBytesTotal > 0) {
            const float frac = static_cast<float>(static_cast<double>(p.stageBytesDone) /
                                                  static_cast<double>(p.stageBytesTotal));
            char overlay[80];
            std::snprintf(overlay, sizeof(overlay), "%s / %s",
                          formatSize(p.stageBytesDone).c_str(),
                          formatSize(p.stageBytesTotal).c_str());
            ImGui::SetNextItemWidth(240);
            ImGui::ProgressBar(frac > 1.0f ? 1.0f : frac, ImVec2(240, 0), overlay);
            ImGui::SameLine();
            ImGui::TextColored(kDim, "  %s / %s files   %s candidates",
                               formatCount(p.done).c_str(), formatCount(p.total).c_str(),
                               formatCount(p.candidates).c_str());
        } else if (p.total > 0) {
            const float frac = static_cast<float>(static_cast<double>(p.done) /
                                                  static_cast<double>(p.total));
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%s / %s", formatCount(p.done).c_str(),
                          formatCount(p.total).c_str());
            ImGui::SetNextItemWidth(240);
            ImGui::ProgressBar(frac, ImVec2(240, 0), overlay);
            ImGui::SameLine();
            ImGui::TextColored(kDim, "  %s candidates   %s read",
                               formatCount(p.candidates).c_str(),
                               formatSize(p.bytesRead).c_str());
        } else {
            ImGui::TextDisabled("%s files", formatCount(p.done).c_str());
            ImGui::SameLine();
            ImGui::TextColored(kDim, "  %s candidates   %s read",
                               formatCount(p.candidates).c_str(),
                               formatSize(p.bytesRead).c_str());
        }

        if (!p.current.empty()) {
            ImGui::TextColored(kDim, "reading %s", p.current.c_str());
        }
    } else if (s.haveResults) {
        const Totals& t = s.totals;
        if (t.uniques > 0) {
            ImGui::TextColored(kDim,
                               "%s files scanned   %s with no copy anywhere   read %s in %s",
                               formatCount(s.stats.walk.filesSeen).c_str(),
                               formatCount(t.uniques).c_str(),
                               formatSize(s.stats.bytesRead).c_str(),
                               formatDuration(s.stats.seconds).c_str());
        } else {
            ImGui::TextColored(kDim,
                               "%s files scanned   %s groups   %s extras   %s reclaimable   "
                               "read %s in %s",
                               formatCount(s.stats.walk.filesSeen).c_str(),
                               formatCount(t.groups).c_str(), formatCount(t.extras).c_str(),
                               formatSize(t.reclaimable).c_str(),
                               formatSize(s.stats.bytesRead).c_str(),
                               formatDuration(s.stats.seconds).c_str());
        }
    } else {
        ImGui::TextColored(kDim, "no scan yet");
    }

    if (!s.notice.empty()) {
        ImGui::TextColored(kWarn, "%s", s.notice.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("ok")) s.notice.clear();
    }
}

// Pulls finished work out of the background threads. Called once a frame so
// there is exactly one place where results and run summaries land.
void collectBackgroundWork(AppState& s) {
    if (s.engine.takeResults(s.files, s.groups, s.stats)) {
        s.haveResults = true;
        s.expanded.assign(s.groups.size(), 0);
        s.rowsDirty = true;
        if (s.stats.cancelled) {
            s.notice = "scan cancelled, no results";
        } else if (s.groups.empty()) {
            s.notice = s.pipeline.report == ReportMode::Uniques
                           ? "every file has a copy somewhere in the inputs"
                           : "no duplicates found";
        }
    }

    RunSummary summary;
    if (s.actions.takeSummary(summary)) {
        s.lastRun = summary;
        s.haveLastRun = true;
        s.runsDirty = true;

        // The list is corrected in place rather than by rescanning: the files
        // that went are known exactly, and everything else is unchanged.
        const std::unordered_set<std::string> removed(summary.removed.begin(),
                                                      summary.removed.end());
        pruneRemoved(s.groups, s.files, removed);
        s.expanded.assign(s.groups.size(), 0);
        s.rowsDirty = true;

        char msg[256];
        std::snprintf(msg, sizeof(msg), "%s: %zu file(s), %s reclaimed, %zu failed%s",
                      actionKindName(summary.kind), summary.done,
                      formatSize(summary.bytes).c_str(), summary.failed,
                      summary.cancelled ? ", cancelled" : "");
        s.notice = msg;
    }
}

void drawApplyConfirm(AppState& s) {
    if (s.askApplyConfirm) {
        ImGui::OpenPopup("Confirm");
        s.askApplyConfirm = false;
    }

    if (!ImGui::BeginPopupModal("Confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const Totals& t = s.totals;
    if (t.uniques > 0) {
        ImGui::TextColored(kBad, "These %s file(s) have no other copy anywhere in the inputs.",
                           formatCount(t.selected).c_str());
        ImGui::TextColored(kDim, "Moving them removes them from where they are. The run is");
        ImGui::TextColored(kDim, "recorded, so it can be put back.");
        ImGui::Spacing();
    }
    if (s.action == ActionKind::Delete) {
        ImGui::TextColored(kBad, "Permanently delete %s file(s), freeing %s.",
                           formatCount(t.selected).c_str(), formatSize(t.selectedBytes).c_str());
        ImGui::TextColored(kDim, "This cannot be undone. Every group keeps exactly one copy.");
    } else {
        ImGui::TextUnformatted("Move");
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "%s file(s), %s", formatCount(t.selected).c_str(),
                           formatSize(t.selectedBytes).c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted("to");
        ImGui::SameLine();
        ImGui::TextColored(kCyan, "%s", s.quarantineRoot.c_str());
        ImGui::TextColored(kDim, "Each file keeps its original path under that folder, and the");
        ImGui::TextColored(kDim, "run is recorded so it can be restored.");
    }

    ImGui::Separator();
    if (ImGui::Button("Apply", ImVec2(120, 0))) {
        s.actions.start(collectSelected(s), s.action, s.quarantineRoot, &s.log);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void drawRestoreConfirm(AppState& s) {
    if (s.askRestoreIndex >= 0 && !ImGui::IsPopupOpen("Restore")) ImGui::OpenPopup("Restore");
    if (!ImGui::BeginPopupModal("Restore", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (s.askRestoreIndex < 0 || s.askRestoreIndex >= static_cast<int>(s.runs.size())) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const RunEntry& run = s.runs[s.askRestoreIndex];
    ImGui::Text("Put %s file(s) back where they came from?", formatCount(run.files).c_str());
    ImGui::TextColored(kDim, "from %s", run.root.c_str());
    ImGui::TextColored(kDim, "Any original path that is occupied again is left alone.");

    ImGui::Separator();
    if (ImGui::Button("Restore", ImVec2(120, 0))) {
        s.lastRestore = restoreRun(run, &s.log);
        s.haveLastRestore = true;
        s.runsDirty = true;
        char msg[192];
        std::snprintf(msg, sizeof(msg), "restored %llu, skipped %llu, failed %llu",
                      static_cast<unsigned long long>(s.lastRestore.restored),
                      static_cast<unsigned long long>(s.lastRestore.skipped),
                      static_cast<unsigned long long>(s.lastRestore.failed));
        s.notice = msg;
        s.askRestoreIndex = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        s.askRestoreIndex = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace

void startScan(AppState& s) {
    const std::vector<std::string> clean = normalizeRoots(s.roots);
    if (clean.empty()) {
        s.notice = "no usable input directories";
        return;
    }
    if (clean.size() != s.roots.size()) {
        s.notice = "some input directories were nested inside others and were folded in";
    }

    s.scanRoots = clean;
    s.haveResults = false;
    s.groups.clear();
    s.files.clear();
    s.rows.clear();
    s.expanded.clear();
    s.rowsDirty = true;
    // The log is deliberately not cleared: a scan, an apply and a restore are
    // one session's story, and the timestamps keep them apart.
    s.engine.start(clean, s.scope, s.pipeline, s.tie, &s.cache, &s.log);
}

std::vector<ActionItem> collectSelected(const AppState& s) {
    std::vector<ActionItem> out;
    for (const auto& g : s.groups) {
        if (g.members.empty()) continue;
        const FileEntry& keeper = s.files[g.members[g.keeper].fileIndex];
        for (size_t i = 0; i < g.members.size(); ++i) {
            if (!g.members[i].selected) continue;
            // Belt and braces on a duplicate group; a unique row has no keeper
            // to skip, and its one file is the thing being acted on.
            if (!g.unique && static_cast<int>(i) == g.keeper) continue;

            const FileEntry& f = s.files[g.members[i].fileIndex];
            ActionItem item;
            item.path = f.path;
            item.keeper = g.unique ? std::string() : keeper.path;
            item.size = f.size;
            item.mtime = f.mtime;
            item.keeperSize = g.unique ? 0 : keeper.size;
            item.keeperMtime = g.unique ? 0 : keeper.mtime;
            item.hash = f.fullHash;
            out.push_back(std::move(item));
        }
    }
    return out;
}

void handleDrops(AppState& s) {
    if (s.droppedPaths.empty()) return;
    for (const auto& raw : s.droppedPaths) {
        const std::string p = normalizePath(raw);
        if (p.empty()) continue;
        if (std::find(s.roots.begin(), s.roots.end(), p) == s.roots.end()) s.roots.push_back(p);
    }
    s.droppedPaths.clear();
}

SessionData sessionFromState(const AppState& s) {
    SessionData d;
    d.roots = s.roots;
    d.scope = s.scope;
    d.pipeline = s.pipeline;
    d.tie = s.tie;
    d.quarantineRoot = s.quarantineRoot;
    d.action = s.action;
    d.sort = s.sort;
    d.filter = s.filter;
    d.showLog = s.showLog;
    return d;
}

void applySession(AppState& s, const SessionData& d) {
    s.roots = d.roots;
    s.scope = d.scope;
    s.pipeline = d.pipeline;
    s.tie = d.tie;
    s.quarantineRoot = d.quarantineRoot;
    s.action = d.action;
    s.sort = d.sort;
    s.filter = d.filter;
    s.showLog = d.showLog;

    std::snprintf(s.filterBuf, sizeof(s.filterBuf), "%s", s.filter.text.c_str());
    std::snprintf(s.quarBuf, sizeof(s.quarBuf), "%s", s.quarantineRoot.c_str());
}

void drawUi(AppState& s) {
    collectBackgroundWork(s);
    s.totals = computeTotals(s.groups, s.files);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("duplicates-ui", nullptr, flags);

    drawTopBar(s);
    ImGui::Separator();

    // Inputs on the left, everything that shapes the scan on the right.
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginChild("inputs", ImVec2(half, 320));
    drawInputs(s);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("pipeline", ImVec2(half, 320));
    drawPipeline(s);
    ImGui::EndChild();

    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Duplicates")) {
            // The action bar is pinned to the bottom, so the list gets whatever
            // is left rather than pushing the buttons off screen. Two rows: the
            // controls, and the line that explains a disabled Apply.
            drawResults(s, ImGui::GetFrameHeightWithSpacing() * 2.0f);
            drawActionBar(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Runs")) {
            drawRunsTab(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            drawLogTab(s);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    std::string picked;
    if (s.browser.draw(picked)) {
        switch (s.browserTarget) {
            case BrowserTarget::InputRoot:
                if (std::find(s.roots.begin(), s.roots.end(), picked) == s.roots.end()) {
                    s.roots.push_back(picked);
                }
                break;
            case BrowserTarget::Quarantine:
                s.quarantineRoot = picked;
                std::snprintf(s.quarBuf, sizeof(s.quarBuf), "%s", picked.c_str());
                s.runsDirty = true;
                break;
            case BrowserTarget::None: break;
        }
        s.browserTarget = BrowserTarget::None;
    }

    drawApplyConfirm(s);
    drawRestoreConfirm(s);

    ImGui::End();
}

void applyTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.ChildRounding = 0.0f;
    st.FrameRounding = 0.0f;
    st.PopupRounding = 0.0f;
    st.GrabRounding = 0.0f;
    st.ScrollbarRounding = 0.0f;
    st.TabRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.WindowPadding = ImVec2(10, 8);
    st.FramePadding = ImVec2(6, 3);
    st.ItemSpacing = ImVec2(6, 5);
    st.CellPadding = ImVec2(6, 3);

    ImVec4* c = st.Colors;
    const ImVec4 black(0, 0, 0, 1);
    const ImVec4 line(0.18f, 0.18f, 0.18f, 1);
    c[ImGuiCol_WindowBg] = black;
    c[ImGuiCol_ChildBg] = black;
    c[ImGuiCol_PopupBg] = black;
    c[ImGuiCol_MenuBarBg] = black;
    c[ImGuiCol_Text] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_TextDisabled] = kDim;
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = ImVec4(0.07f, 0.07f, 0.07f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.15f, 0.15f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.22f, 1);
    c[ImGuiCol_Button] = ImVec4(0.10f, 0.10f, 0.10f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.30f, 0.30f, 1);
    c[ImGuiCol_Header] = ImVec4(0.14f, 0.14f, 0.14f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.28f, 0.28f, 0.28f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_SliderGrab] = ImVec4(0.60f, 0.60f, 0.60f, 1);
    c[ImGuiCol_ScrollbarBg] = black;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.22f, 0.22f, 1);
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_Tab] = ImVec4(0.06f, 0.06f, 0.06f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(0.22f, 0.22f, 0.22f, 1);
    c[ImGuiCol_TabSelected] = ImVec4(0.16f, 0.16f, 0.16f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.06f, 0.06f, 0.06f, 1);
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.12f, 0.12f, 0.12f, 1);
    c[ImGuiCol_TableRowBg] = black;
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.03f, 0.03f, 0.03f, 1);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.85f, 0.85f, 0.85f, 1);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.65f);
}
