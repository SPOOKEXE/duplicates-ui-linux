#include <cstdio>
#include <cstring>

#include "scanner.h"
#include "ui.h"
#include "util.h"

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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.roots[i].c_str());

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

void drawScopeAndStages(AppState& s) {
    ImGui::TextUnformatted("MATCHING");
    ImGui::SameLine();
    ImGui::TextColored(kDim, " every stage only sees what the one before it kept");

    ImGui::Checkbox("first", &s.stages.headBytes);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "one seek per file, and where nearly every same-size coincidence dies\n"
            "a file no bigger than this is hashed in full here and skips the next stage");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::BeginDisabled(!s.stages.headBytes);
    if (ImGui::InputScalar("##headsize", ImGuiDataType_U64, &s.stages.headSize)) {
        if (s.stages.headSize < 512) s.stages.headSize = 512;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("bytes");

    ImGui::SameLine(0, 20);
    ImGui::Checkbox("full content hash", &s.stages.fullHash);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("streams the whole file, and skips it entirely on a cache hit");
    }

    ImGui::SameLine(0, 20);
    ImGui::Checkbox("exact byte compare", &s.stages.exactCompare);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "the only stage that proves a match rather than strongly suggesting one\n"
            "turning it off means trusting a 64-bit hash with an irreversible delete");
    }

    ImGui::Checkbox("same filename", &s.stages.sameName);
    ImGui::SameLine(0, 20);
    ImGui::Checkbox("same mtime", &s.stages.sameMtime);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("these only narrow the result, they can never add a false match");
    }
    ImGui::SameLine(0, 20);
    ImGui::TextUnformatted("threads");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::SliderInt("##threads", &s.stages.hashThreads, 1, 16);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("a large win on an SSD, a loss on one spinning disk");
    }

    if (!s.stages.fullHash && !s.stages.exactCompare) {
        ImGui::TextColored(kBad,
                           "with neither full hash nor byte compare, files are matched on size "
                           "alone");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("SCOPE");

    ImGui::TextUnformatted("smallest file");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputScalar("##minsize", ImGuiDataType_U64, &s.scope.minSize);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "bytes");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("1 keeps zero-byte files out, which are all identical to each other");
    }

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

    ImGui::TextUnformatted("keep");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
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

    ImGui::SameLine(0, 20);
    ImGui::TextUnformatted("exclude");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##globs", "*.tmp, /mnt/scratch/*", s.globBuf,
                                 sizeof(s.globBuf))) {
        s.scope.excludeGlobs = s.globBuf;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("comma separated globs, matched against the path and the filename");
    }
}
