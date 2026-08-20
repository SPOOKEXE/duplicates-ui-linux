#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "scanner.h"
#include "ui.h"
#include "util.h"

// After ui.h, which brings in imgui.h: this header needs it already declared.
#include <imgui_stdlib.h>

namespace {

void clearBuf(char* buf, size_t cap) { std::memset(buf, 0, cap); }

// Both the typed box and the picker end up here, so a path is normalised and
// deduplicated in exactly one place.
void addRoot(AppState& s, const std::string& raw) {
    const std::string p = normalizePath(raw);
    if (p.empty()) return;
    for (const auto& existing : s.roots) {
        if (existing == p) return;
    }
    s.roots.push_back(p);
}

}  // namespace

void drawInputs(AppState& s) {
    ImGui::TextUnformatted("INPUT DIRECTORIES");
    ImGui::SameLine();
    ImGui::TextColored(kDim, " priority order, the first one keeps its copy");

    ImGui::BeginChild("rootlist", ImVec2(0, 120), true);

    int removeAt = -1;
    int moveFrom = -1, moveTo = -1;

    for (size_t i = 0; i < s.roots.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::SmallButton("^")) {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) - 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1 >= s.roots.size());
        if (ImGui::SmallButton("v")) {
            moveFrom = static_cast<int>(i);
            moveTo = static_cast<int>(i) + 1;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::TextColored(i == 0 ? kOk : kDim, "%zu", i + 1);
        ImGui::SameLine();

        char label[1200];
        std::snprintf(label, sizeof(label), "%s", s.roots[i].c_str());
        ImGui::Selectable(label, false);
        // Double click, not single: a single click on this row is the start of
        // a drag, which is how priority is reordered.
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (!openInFileManager(s.roots[i])) {
                    s.notice = "could not open " + s.roots[i] + "; is xdg-open installed?";
                }
            }
            ImGui::SetTooltip("%s\ndouble click to open it in your file manager",
                              s.roots[i].c_str());
        }

        // Dragging a row onto another is the natural way to express priority,
        // and the arrows stay for keyboards and for precise single steps.
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
            const int from = static_cast<int>(i);
            ImGui::SetDragDropPayload("ROOT_ROW", &from, sizeof(int));
            ImGui::TextUnformatted(s.roots[i].c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ROOT_ROW")) {
                moveFrom = *static_cast<const int*>(payload->Data);
                moveTo = static_cast<int>(i);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }

    if (s.roots.empty()) {
        ImGui::TextColored(kDim, "drop folders here, or add one below");
    }
    ImGui::EndChild();

    // Applied after the loop, so the vector is never resized mid-iteration.
    if (removeAt >= 0) s.roots.erase(s.roots.begin() + removeAt);
    if (moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo &&
        moveTo < static_cast<int>(s.roots.size())) {
        std::string moved = s.roots[moveFrom];
        s.roots.erase(s.roots.begin() + moveFrom);
        s.roots.insert(s.roots.begin() + moveTo, std::move(moved));
    }

    ImGui::SetNextItemWidth(-160);
    const bool entered = ImGui::InputTextWithHint("##root", "/path/to/a/folder", s.rootBuf,
                                                 sizeof(s.rootBuf),
                                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool clicked = ImGui::Button("Add", ImVec2(60, 0));
    if (entered || clicked) {
        addRoot(s, s.rootBuf);
        clearBuf(s.rootBuf, sizeof(s.rootBuf));
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(80, 0))) {
        s.browserTarget = BrowserTarget::InputRoot;
        s.browser.open("Add an input directory", s.roots.empty() ? homeDir() : s.roots.back());
    }
}

namespace {

// A new row of each kind starts with the number that makes it useful straight
// away, so adding one never means also remembering to set a size.
Rule freshRule(RuleKind kind) {
    switch (kind) {
        case RuleKind::MinSize: return Rule {true, kind, {}, 1};
        case RuleKind::MaxSize: return Rule {true, kind, {}, 0};
        case RuleKind::HeadBytes: return Rule {true, kind, {}, 65536};
        default: return Rule {true, kind, {}, 0};
    }
}

// The editable part of a row: a pattern box, a byte count, or nothing at all.
void drawRuleParam(Rule& r) {
    switch (r.kind) {
        case RuleKind::Glob:
        case RuleKind::Regex: {
            ImGui::SetNextItemWidth(-190);
            ImGui::InputTextWithHint("##pat",
                                     r.kind == RuleKind::Glob ? "*.zip" : "\\.(zip|rar|7z)$",
                                     &r.pattern);
            break;
        }
        case RuleKind::MinSize:
        case RuleKind::MaxSize:
        case RuleKind::HeadBytes: {
            ImGui::SetNextItemWidth(120);
            ImGui::InputScalar("##num", ImGuiDataType_U64, &r.number);
            ImGui::SameLine();
            ImGui::TextColored(kDim, "bytes");
            break;
        }
        default: break;
    }
}

void drawRuleRow(AppState& s, size_t i, int& removeAt, int& moveFrom, int& moveTo) {
    Rule& r = s.pipeline.rules[i];
    ImGui::PushID(static_cast<int>(i));

    ImGui::Checkbox("##on", &r.enabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ruleKindHint(r.kind));

    ImGui::SameLine();
    ImGui::BeginDisabled(i == 0);
    if (ImGui::SmallButton("^")) {
        moveFrom = static_cast<int>(i);
        moveTo = static_cast<int>(i) - 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 >= s.pipeline.rules.size());
    if (ImGui::SmallButton("v")) {
        moveFrom = static_cast<int>(i);
        moveTo = static_cast<int>(i) + 1;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);

    ImGui::SameLine();
    // Glob and regex are the same row wearing a different hat, so the name is
    // also the button that swaps between them. Every other kind is fixed:
    // changing it would leave its number meaning something else.
    const bool isPattern = (r.kind == RuleKind::Glob || r.kind == RuleKind::Regex);
    if (isPattern) {
        if (ImGui::SmallButton(r.kind == RuleKind::Glob ? "glob " : "regex")) {
            r.kind = (r.kind == RuleKind::Glob) ? RuleKind::Regex : RuleKind::Glob;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("click to switch this row to the other kind");
    } else {
        ImGui::TextUnformatted(ruleKindName(r.kind));
    }

    ImGui::SameLine(230);
    drawRuleParam(r);

    // A regex that will not build is marked where it is typed, not discovered
    // halfway through a scan.
    std::string err;
    if (r.kind == RuleKind::Regex && !regexIsValid(r.pattern, err)) {
        ImGui::SameLine();
        ImGui::TextColored(kBad, "bad");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", err.c_str());
    }

    ImGui::SameLine(ImGui::GetContentRegionMax().x - 50);
    ImGui::TextColored(kDim, "%s", ruleIsDrop(r.kind) ? "drop" : "split");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(ruleIsDrop(r.kind)
                              ? "looks at one file and keeps or drops it\n"
                                "applied during the walk, wherever it sits in the list"
                              : "cuts the surviving candidates into smaller sets\n"
                                "runs in list order, and only sees what the row above kept");
    }
    ImGui::PopID();
}

}  // namespace

void drawPipeline(AppState& s) {
    ImGui::TextUnformatted("PIPELINE");
    ImGui::SameLine();
    ImGui::TextColored(kDim, " top to bottom, each row only sees what the one above kept");

    ImGui::SameLine(ImGui::GetContentRegionMax().x - 150);
    ImGui::TextUnformatted("threads");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::SliderInt("##threads", &s.pipeline.threads, 1, 16);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("a large win on an SSD, a loss on one spinning disk");
    }

    ImGui::BeginChild("rulelist", ImVec2(0, 150), true);

    // Size is not a row: it comes free from the walk's lstat, and it is what
    // keeps an exact byte compare from reading every file against every other.
    ImGui::TextColored(kDim, "    size");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 50);
    ImGui::TextColored(kDim, "always");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("free, and what makes every row below it affordable, so it cannot move");
    }

    int removeAt = -1;
    int moveFrom = -1, moveTo = -1;
    for (size_t i = 0; i < s.pipeline.rules.size(); ++i) {
        drawRuleRow(s, i, removeAt, moveFrom, moveTo);
    }
    if (s.pipeline.rules.empty()) {
        ImGui::TextColored(kWarn, "no rules: files are matched on size alone");
    }
    ImGui::EndChild();

    // Applied after the loop, so the vector is never resized mid-iteration.
    if (removeAt >= 0) s.pipeline.rules.erase(s.pipeline.rules.begin() + removeAt);
    if (moveFrom >= 0 && moveTo >= 0 && moveTo < static_cast<int>(s.pipeline.rules.size())) {
        std::swap(s.pipeline.rules[moveFrom], s.pipeline.rules[moveTo]);
    }

    ImGui::SetNextItemWidth(160);
    ImGui::Combo("##newrule", &s.newRuleKind, kRuleKindNames, kRuleKindCount);
    ImGui::SameLine();
    if (ImGui::Button("Add", ImVec2(60, 0))) {
        s.pipeline.rules.push_back(freshRule(static_cast<RuleKind>(s.newRuleKind)));
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(70, 0))) s.pipeline = defaultPipeline();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("back to the default pipeline");

    // These two describe the glob and regex rows as a set, which is not a
    // per-row question, so they sit under the list rather than in it.
    ImGui::SameLine(0, 20);
    ImGui::TextColored(kDim, "patterns");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    int combine = static_cast<int>(s.pipeline.combine);
    if (ImGui::Combo("##combine", &combine, kPatternCombineNames, 2)) {
        s.pipeline.combine = static_cast<PatternCombine>(combine);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("OR: a file matches if any pattern does\nAND: only if every one does");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    int select = static_cast<int>(s.pipeline.select);
    if (ImGui::Combo("##select", &select, kPatternSelectNames, 2)) {
        s.pipeline.select = static_cast<PatternSelect>(select);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "INCLUDE: only matching files are scanned\n"
            "EXCLUDE: matching files are skipped, and a matching folder is skipped whole\n"
            "with no patterns at all, both keep everything");
    }

    const CompiledPipeline compiled = compilePipeline(s.pipeline);
    if (compiled.splits.empty()) {
        ImGui::TextColored(kBad, "no matching row: files would be grouped on size alone");
    } else if (!compiled.problems.empty()) {
        ImGui::TextColored(kWarn, "%s", compiled.problems.front().c_str());
    } else {
        bool proves = false;
        for (const auto& r : compiled.splits) {
            if (r.kind == RuleKind::ExactBytes) proves = true;
        }
        if (!proves) {
            ImGui::TextColored(kWarn,
                               "without an exact byte compare, an irreversible delete rests on a "
                               "64-bit hash");
        } else {
            ImGui::TextColored(kDim, "%s", describePipeline(compiled).c_str());
        }
    }

    ImGui::Separator();
    ImGui::TextUnformatted("SCOPE");
    ImGui::SameLine(0, 20);
    ImGui::Checkbox("hidden files", &s.scope.includeHidden);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("off by default: a .git object store is thousands of dull duplicates");
    }

    ImGui::SameLine(0, 20);
    ImGui::Checkbox("fold hardlinks", &s.scope.collapseHardlinks);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "paths sharing one inode occupy the disk once, so they are shown as one row\n"
            "off shows every linked path as an ordinary, actionable copy");
    }

    ImGui::SameLine(0, 20);
    ImGui::TextUnformatted("keep");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170);
    int tie = static_cast<int>(s.tie);
    if (ImGui::Combo("##tie", &tie, kTieBreakNames, 5)) {
        s.tie = static_cast<TieBreak>(tie);
        // Priority already decided most keepers; this only settles the ties, so
        // the pinned groups are deliberately left alone.
        resolveKeepers(s.groups, s.files, s.tie);
        s.rowsDirty = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("used when two copies sit in equally ranked directories");
    }
}
