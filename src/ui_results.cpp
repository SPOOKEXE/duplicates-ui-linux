#include <cstdio>
#include <cstring>

#include "ui.h"
#include "util.h"

namespace {

// Steps back off a UTF-8 continuation byte, so a cut never lands inside a
// multi-byte character and leaves a broken glyph behind.
size_t backOffToCharStart(const std::string& s, size_t i) {
    while (i > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

// Shrinks a path to fit a column by removing the middle, which keeps both the
// directory it lives in and the filename readable. Without this, a deep path
// runs straight through the columns to its right.
std::string elideMiddle(const std::string& text, float maxWidth) {
    if (maxWidth <= 0.0f) return text;
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;

    const std::string ellipsis = "...";
    const auto fits = [&](size_t head, size_t tail) {
        const std::string candidate =
            text.substr(0, head) + ellipsis + text.substr(text.size() - tail);
        return ImGui::CalcTextSize(candidate.c_str()).x <= maxWidth;
    };

    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (fits(backOffToCharStart(text, mid / 2),
                 text.size() - backOffToCharStart(text, text.size() - (mid - mid / 2)))) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    if (lo == 0) return ellipsis;

    const size_t head = backOffToCharStart(text, lo / 2);
    const size_t tailStart = backOffToCharStart(text, text.size() - (lo - lo / 2));
    return text.substr(0, head) + ellipsis + text.substr(tailStart);
}

// The label under a member's path when its inode is reachable by other names.
std::string linkNote(const FileEntry& f) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "also linked at %zu other path(s)", f.alsoLinkedAt.size());
    return buf;
}

const char* rootLabel(const AppState& s, const FileEntry& f) {
    if (f.rootIndex < 0 || f.rootIndex >= static_cast<int>(s.scanRoots.size())) return "";
    const std::string& root = s.scanRoots[f.rootIndex];
    const size_t slash = root.find_last_of('/');
    return slash == std::string::npos || slash + 1 >= root.size() ? root.c_str()
                                                                  : root.c_str() + slash + 1;
}

void drawGroupHeader(AppState& s, int groupIndex) {
    DupGroup& g = s.groups[groupIndex];
    const FileEntry& keeper = s.files[g.members[g.keeper].fileIndex];
    const uint64_t reclaim = g.unique ? g.size : (g.members.size() - 1) * g.size;

    const bool open = s.expanded[groupIndex] != 0;
    if (ImGui::ArrowButton("##expand", open ? ImGuiDir_Down : ImGuiDir_Right)) {
        s.expanded[groupIndex] = open ? 0 : 1;
        // The row list is rebuilt at the top of the next frame; this frame keeps
        // drawing the old one, which is invisible to the user.
        s.rowsDirty = true;
    }

    ImGui::SameLine();
    ImGui::Text("%s", formatSize(g.size).c_str());
    ImGui::SameLine(140);
    if (g.unique) {
        ImGui::TextColored(kWarn, "only");
    } else {
        ImGui::TextColored(kDim, "x%zu", g.members.size());
    }
    ImGui::SameLine(190);

    const std::string name = keeper.path.substr(keeper.path.find_last_of('/') + 1);
    ImGui::TextUnformatted(
        elideMiddle(name, ImGui::GetWindowContentRegionMax().x - 130.0f - 190.0f).c_str());
    if (g.userPinned) {
        ImGui::SameLine();
        ImGui::TextColored(kWarn, "[pinned]");
    }

    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 120);
    ImGui::TextColored(kDim, "%s", formatSize(reclaim).c_str());
}

void drawMemberRow(AppState& s, int groupIndex, int memberIndex) {
    DupGroup& g = s.groups[groupIndex];
    Member& m = g.members[memberIndex];
    const FileEntry& f = s.files[m.fileIndex];
    const bool isKeeper = (memberIndex == g.keeper);

    ImGui::Indent(20.0f);

    // A unique file has no sibling to keep instead of it, so there is no keeper
    // dot to offer: the only choice is whether to act on the file itself.
    if (!g.unique) {
        if (ImGui::RadioButton("##keep", isKeeper)) setKeeper(g, memberIndex);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("keep this copy, and pin the choice");
        ImGui::SameLine();
    }

    if (!g.unique && isKeeper) {
        ImGui::TextColored(kOk, "keep");
    } else {
        bool selected = m.selected;
        if (ImGui::Checkbox("##sel", &selected)) m.selected = selected;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(g.unique ? "move this file when you apply; nothing else holds a copy"
                                       : "remove this copy when you apply");
        }
        if (g.unique) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "only copy");
        }
    }

    const float pathStart = 110.0f;
    const float pathWidth = ImGui::GetWindowContentRegionMax().x - 250.0f - pathStart;

    ImGui::SameLine(pathStart);
    ImGui::TextColored(isKeeper ? ImVec4(1, 1, 1, 1) : kDim, "%s",
                       elideMiddle(f.path, pathWidth).c_str());
    if (ImGui::IsItemHovered()) {
        // The folder rather than the file: opening a duplicate in whatever
        // application owns it is rarely what someone checking a copy wants.
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (!openContainingFolder(f.path)) {
                s.notice = "could not open the folder; is xdg-open installed?";
            }
        }
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(f.path.c_str());
        for (const auto& link : f.alsoLinkedAt) ImGui::TextColored(kDim, "linked: %s", link.c_str());
        ImGui::TextColored(kDim, "double click to open the containing folder");
        ImGui::EndTooltip();
    }

    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 240);
    ImGui::TextColored(kDim, "%s", formatTime(f.mtime).c_str());
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 110);
    ImGui::TextColored(kCyan, "%s", rootLabel(s, f));

    if (!f.alsoLinkedAt.empty()) {
        ImGui::Indent(110.0f);
        ImGui::TextColored(kDim, "%s", linkNote(f).c_str());
        ImGui::Unindent(110.0f);
    }

    ImGui::Unindent(20.0f);
}

void drawToolbar(AppState& s) {
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputTextWithHint("##filter", "filter by path", s.filterBuf,
                                 sizeof(s.filterBuf))) {
        s.filter.text = s.filterBuf;
        s.rowsDirty = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("bigger than");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputScalar("##fmin", ImGuiDataType_U64, &s.filter.minSize)) s.rowsDirty = true;

    ImGui::SameLine();
    ImGui::TextUnformatted("copies");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##fmembers", &s.filter.minMembers)) {
        if (s.filter.minMembers < 2) s.filter.minMembers = 2;
        s.rowsDirty = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("sort");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    int sort = static_cast<int>(s.sort);
    if (ImGui::Combo("##sort", &sort, kGroupSortNames, 4)) {
        s.sort = static_cast<GroupSort>(sort);
        sortGroups(s.groups, s.files, s.sort);
        s.expanded.assign(s.groups.size(), 0);
        s.rowsDirty = true;
    }

    if (ImGui::Button("Select extras")) selectAllExtras(s.groups);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("tick every copy that is not the keeper, in every group");
    }
    ImGui::SameLine();
    if (ImGui::Button("Deselect all")) deselectAll(s.groups);
    ImGui::SameLine();
    if (ImGui::Button("Invert")) invertSelection(s.groups);
    ImGui::SameLine();
    if (ImGui::Button("Clear pins")) {
        clearPins(s.groups);
        resolveKeepers(s.groups, s.files, s.tie);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("hand-picked keepers go back to following the priority order");
    }
    ImGui::SameLine();
    if (ImGui::Button("Expand all")) {
        s.expanded.assign(s.groups.size(), 1);
        s.rowsDirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Collapse all")) {
        s.expanded.assign(s.groups.size(), 0);
        s.rowsDirty = true;
    }

    ImGui::SameLine();
    if (s.totals.uniques > 0) {
        ImGui::TextColored(kWarn, "  |  %s file(s) with no copy anywhere",
                           formatCount(s.totals.uniques).c_str());
    } else {
        ImGui::TextColored(kDim, "  |  %s groups   %s extras   %s reclaimable",
                           formatCount(s.totals.groups).c_str(),
                           formatCount(s.totals.extras).c_str(),
                           formatSize(s.totals.reclaimable).c_str());
    }
}

}  // namespace

void rebuildRows(AppState& s) {
    if (s.expanded.size() != s.groups.size()) s.expanded.assign(s.groups.size(), 0);

    s.rows.clear();
    for (size_t g = 0; g < s.groups.size(); ++g) {
        if (!groupMatches(s.groups[g], s.files, s.filter)) continue;
        s.rows.push_back(ResultRow {static_cast<int>(g), -1});
        if (!s.expanded[g]) continue;
        for (size_t m = 0; m < s.groups[g].members.size(); ++m) {
            s.rows.push_back(ResultRow {static_cast<int>(g), static_cast<int>(m)});
        }
    }
    s.rowsDirty = false;
}

void drawResults(AppState& s, float reserveBottom) {
    drawToolbar(s);
    if (s.rowsDirty) rebuildRows(s);

    // Measured here rather than by the caller: the toolbar above has already
    // taken its share of the region, and it is two rows tall.
    float height = ImGui::GetContentRegionAvail().y - reserveBottom;
    if (height < 80.0f) height = 80.0f;
    ImGui::BeginChild("results", ImVec2(0, height), true);

    if (s.rows.empty()) {
        if (!s.haveResults) {
            ImGui::TextColored(kDim, "run a scan to see duplicate groups here");
        } else if (s.groups.empty()) {
            ImGui::TextColored(kDim, s.pipeline.report == ReportMode::Uniques
                                         ? "every file in these directories has a copy"
                                         : "no duplicates found in these directories");
        } else {
            ImGui::TextColored(kDim, "no group matches the current filter");
        }
        ImGui::EndChild();
        return;
    }

    // Only the rows on screen are drawn, so a hundred thousand groups scroll for
    // the same cost as ten.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(s.rows.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const ResultRow row = s.rows[i];
            if (row.group >= static_cast<int>(s.groups.size())) continue;

            ImGui::PushID(i);
            if (row.member < 0) {
                drawGroupHeader(s, row.group);
            } else if (row.member < static_cast<int>(s.groups[row.group].members.size())) {
                drawMemberRow(s, row.group, row.member);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}
