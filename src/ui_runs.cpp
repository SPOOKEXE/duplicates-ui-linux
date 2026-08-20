#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"
#include "ui.h"
#include "util.h"

namespace {

// A quarantine folder inside a scanned directory would re-import everything it
// just took out on the next scan, and could even be offered as a duplicate of
// the file it is protecting.
bool quarantineInsideAnInput(const AppState& s) {
    if (s.quarantineRoot.empty()) return false;
    for (const auto& root : s.roots) {
        if (root == s.quarantineRoot || pathIsUnder(root, s.quarantineRoot)) return true;
    }
    return false;
}

}  // namespace

void drawActionBar(AppState& s) {
    const ActionProgress ap = s.actions.progress();

    if (ap.running) {
        const float frac = ap.total > 0 ? static_cast<float>(static_cast<double>(ap.done) /
                                                             static_cast<double>(ap.total))
                                        : 0.0f;
        char overlay[96];
        std::snprintf(overlay, sizeof(overlay), "%zu / %zu", ap.done, ap.total);
        ImGui::ProgressBar(frac, ImVec2(280, 0), overlay);
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(80, 0))) s.actions.cancel();
        ImGui::SameLine();
        ImGui::TextColored(kDim, "%s   %s freed   %zu failed", ap.current.c_str(),
                           formatSize(ap.bytes).c_str(), ap.failed);
        return;
    }

    bool quarantine = (s.action == ActionKind::Quarantine);
    bool deleting = (s.action == ActionKind::Delete);

    // Two checkboxes, but only one meaning: a destructive run has to be
    // unambiguous, and "delete some, quarantine others" is not a thing anyone
    // wants from a single button.
    if (ImGui::Checkbox("move to quarantine", &quarantine)) {
        s.action = quarantine ? ActionKind::Quarantine : ActionKind::Delete;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("##quar", "/path/to/quarantine", s.quarBuf, sizeof(s.quarBuf))) {
        s.quarantineRoot = normalizePath(s.quarBuf);
        // Manifests live with the files they describe, so a different folder is
        // a different history.
        s.runsDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse##quar", ImVec2(80, 0))) {
        s.browserTarget = BrowserTarget::Quarantine;
        s.browser.open("Choose a quarantine folder", s.quarantineRoot);
    }

    // Deleting a file that has no other copy is the one thing this tool exists
    // to prevent, so a uniques run cannot reach the delete at all.
    const bool onlyCopies = s.totals.uniques > 0;
    ImGui::SameLine(0, 24);
    ImGui::BeginDisabled(onlyCopies);
    if (ImGui::Checkbox("delete permanently", &deleting)) {
        s.action = deleting ? ActionKind::Delete : ActionKind::Quarantine;
    }
    ImGui::EndDisabled();
    if (onlyCopies && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("these files have no other copy, so only the reversible move is offered");
    }
    if (onlyCopies && s.action == ActionKind::Delete) s.action = ActionKind::Quarantine;

    const bool needsRoot = (s.action == ActionKind::Quarantine);
    const bool badRoot = needsRoot && (s.quarantineRoot.empty() || quarantineInsideAnInput(s));
    const bool canApply = s.totals.selected > 0 && !badRoot && !s.engine.running();

    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 320);
    ImGui::BeginDisabled(!canApply);
    char label[128];
    std::snprintf(label, sizeof(label), "Apply to %s file(s), %s",
                  formatCount(s.totals.selected).c_str(),
                  formatSize(s.totals.selectedBytes).c_str());
    if (ImGui::Button(label, ImVec2(310, 0))) s.askApplyConfirm = true;
    ImGui::EndDisabled();

    if (badRoot) {
        ImGui::TextColored(kBad, s.quarantineRoot.empty()
                                     ? "choose a quarantine folder first"
                                     : "the quarantine folder is inside a scanned directory");
    }
}

void drawRunsTab(AppState& s) {
    if (s.runsDirty) {
        s.runs = loadRuns(s.quarantineRoot);
        s.runsDirty = false;
    }

    if (ImGui::Button("Refresh", ImVec2(90, 0))) s.runsDirty = true;
    ImGui::SameLine();
    ImGui::TextColored(kDim,
                       "quarantine runs can be put back; a delete run is recorded but is gone");

    // Only reserve room at the bottom when there is actually something to put
    // there, so an uneventful run list uses the whole tab.
    const ActionProgress ap = s.actions.progress();
    const bool hasFooter = !ap.failures.empty() ||
                           (s.haveLastRestore && !s.lastRestore.notes.empty());
    ImGui::BeginChild("runlist", ImVec2(0, hasFooter ? -130.0f : 0.0f), true);
    if (s.runs.empty()) {
        ImGui::TextColored(kDim, "nothing applied yet");
    }
    for (size_t i = 0; i < s.runs.size(); ++i) {
        const RunEntry& run = s.runs[i];
        ImGui::PushID(static_cast<int>(i));

        ImGui::TextUnformatted(run.stamp.c_str());
        ImGui::SameLine(190);
        ImGui::TextColored(run.kind == ActionKind::Delete ? kBad : kWarn, "%s",
                           actionKindName(run.kind));
        ImGui::SameLine(300);
        ImGui::TextColored(kDim, "%s file(s)   %s", formatCount(run.files).c_str(),
                           formatSize(run.bytes).c_str());
        ImGui::SameLine(500);
        if (run.restored) {
            ImGui::TextColored(kOk, "restored");
        } else if (run.kind == ActionKind::Quarantine) {
            if (ImGui::Button("Restore", ImVec2(90, 0))) s.askRestoreIndex = static_cast<int>(i);
        } else {
            ImGui::TextColored(kDim, "not reversible");
        }
        ImGui::SameLine(620);
        ImGui::TextColored(kDim, "%s", run.root.empty() ? run.manifest.c_str() : run.root.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();

    if (!ap.failures.empty()) {
        ImGui::TextColored(kBad, "last run: %zu row(s) failed", ap.failed);
        ImGui::BeginChild("failures", ImVec2(0, 100), true);
        for (const auto& row : ap.failures) {
            ImGui::TextColored(kBad, "%s", row.error.c_str());
            ImGui::SameLine(360);
            ImGui::TextColored(kDim, "%s", row.item.path.c_str());
        }
        ImGui::EndChild();
    } else if (s.haveLastRestore && !s.lastRestore.notes.empty()) {
        ImGui::TextColored(kWarn, "restore notes");
        ImGui::BeginChild("restorenotes", ImVec2(0, 100), true);
        for (const auto& note : s.lastRestore.notes) {
            ImGui::TextColored(kDim, "%s", note.c_str());
        }
        ImGui::EndChild();
    }
}

void drawLogTab(AppState& s) {
    if (ImGui::Button("Clear", ImVec2(90, 0))) s.log.clear();
    ImGui::SameLine();
    ImGui::Checkbox("info", &s.logShowInfo);
    ImGui::SameLine();
    ImGui::Checkbox("warn", &s.logShowWarn);
    ImGui::SameLine();
    ImGui::Checkbox("error", &s.logShowError);

    ImGui::SameLine(0, 20);
    ImGui::TextColored(kDim, "%zu info   %zu warn   %zu error",
                       s.log.count(LogLevel::Info), s.log.count(LogLevel::Warn),
                       s.log.count(LogLevel::Error));
    const size_t dropped = s.log.dropped();
    if (dropped > 0) {
        ImGui::SameLine(0, 20);
        // Said out loud rather than quietly implied: a truncated log that looks
        // complete is worse than no log.
        ImGui::TextColored(kWarn, "%s older line(s) scrolled off; the run manifest has them all",
                           formatCount(dropped).c_str());
    }

    if (s.haveResults) {
        const WalkStats& w = s.stats.walk;
        ImGui::TextColored(kDim,
                           "last walk skipped: %s hidden   %s below the floor   %s above the "
                           "ceiling   %s symlinks   %s filtered   %s pruned folders   %s folded "
                           "hardlinks   %s unreadable folders",
                           formatCount(w.skippedHidden).c_str(), formatCount(w.skippedSmall).c_str(),
                           formatCount(w.skippedLarge).c_str(),
                           formatCount(w.skippedSymlink).c_str(),
                           formatCount(w.skippedFiltered).c_str(), formatCount(w.prunedDirs).c_str(),
                           formatCount(w.collapsedLinks).c_str(), formatCount(w.dirErrors).c_str());
    }

    // A "moved: <source> -> <destination>" line is two absolute paths long and
    // must stay readable, so this pane scrolls sideways rather than clipping.
    ImGui::BeginChild("log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    const std::vector<LogLine> all = s.log.lines();

    // Filtered up front so the clipper's row count matches what is drawn.
    std::vector<const LogLine*> shown;
    shown.reserve(all.size());
    for (const auto& line : all) {
        const bool want = (line.level == LogLevel::Info && s.logShowInfo) ||
                          (line.level == LogLevel::Warn && s.logShowWarn) ||
                          (line.level == LogLevel::Error && s.logShowError);
        if (want) shown.push_back(&line);
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogLine& line = *shown[i];
            ImGui::TextColored(kDim, "%s", line.stamp.c_str());
            ImGui::SameLine(70);
            switch (line.level) {
                case LogLevel::Error: ImGui::TextColored(kBad, "%s", line.text.c_str()); break;
                case LogLevel::Warn: ImGui::TextColored(kWarn, "%s", line.text.c_str()); break;
                case LogLevel::Info: ImGui::TextUnformatted(line.text.c_str()); break;
            }
        }
    }

    // Only while something is running, so scrolling back through a finished run
    // is not yanked to the bottom by a late line.
    if ((s.engine.running() || s.actions.running()) &&
        ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - ImGui::GetTextLineHeightWithSpacing()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}
