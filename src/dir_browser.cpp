#include "dir_browser.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "util.h"

namespace fs = std::filesystem;

namespace {

void setBuf(char* buf, size_t cap, const std::string& s) {
    std::snprintf(buf, cap, "%s", s.c_str());
}

}  // namespace

void DirBrowser::open(const char* title, const std::string& startPath) {
    title_ = title;
    selected_.clear();
    error_.clear();

    std::error_code ec;
    fs::path start = startPath.empty() ? fs::path(homeDir()) : fs::path(startPath);
    if (!fs::is_directory(start, ec)) start = start.parent_path();
    if (start.empty() || !fs::is_directory(start, ec)) start = fs::path(homeDir());

    cwd_ = start.string();
    setBuf(pathBuf_, sizeof(pathBuf_), cwd_);
    refresh();
    open_ = true;
    needsPopup_ = true;
}

void DirBrowser::refresh() {
    dirs_.clear();
    error_.clear();

    std::error_code ec;
    fs::directory_iterator it(cwd_, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error_ = "cannot read " + cwd_ + ": " + ec.message();
        return;
    }
    for (const auto& entry : it) {
        std::error_code dirEc;
        const std::string name = entry.path().filename().string();
        if (!showHidden_ && !name.empty() && name[0] == '.') continue;
        if (entry.is_directory(dirEc)) dirs_.push_back(name);
    }
    std::sort(dirs_.begin(), dirs_.end());
}

bool DirBrowser::draw(std::string& out) {
    if (!open_) return false;

    if (needsPopup_) {
        ImGui::OpenPopup(title_.c_str());
        needsPopup_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_Appearing);
    bool picked = false;
    bool stayOpen = true;
    if (ImGui::BeginPopupModal(title_.c_str(), &stayOpen, ImGuiWindowFlags_NoSavedSettings)) {
        const auto go = [&] {
            std::error_code ec;
            const std::string typed = normalizePath(pathBuf_);
            if (fs::is_directory(typed, ec)) {
                cwd_ = typed;
                selected_.clear();
                refresh();
            } else {
                error_ = "not a directory";
            }
        };

        ImGui::SetNextItemWidth(-90);
        if (ImGui::InputText("##path", pathBuf_, sizeof(pathBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            go();
        }
        ImGui::SameLine();
        if (ImGui::Button("Go", ImVec2(80, 0))) go();

        ImGui::BeginChild("list", ImVec2(0, -70), true);
        if (fs::path(cwd_).has_parent_path() && fs::path(cwd_).parent_path() != cwd_) {
            if (ImGui::Selectable("..", false, ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(0)) {
                cwd_ = fs::path(cwd_).parent_path().string();
                setBuf(pathBuf_, sizeof(pathBuf_), cwd_);
                selected_.clear();
                refresh();
            }
        }
        for (const auto& d : dirs_) {
            const bool isSel = (selected_ == d);
            if (ImGui::Selectable(d.c_str(), isSel, ImGuiSelectableFlags_AllowDoubleClick)) {
                selected_ = d;
                if (ImGui::IsMouseDoubleClicked(0)) {
                    cwd_ = (fs::path(cwd_) / d).string();
                    setBuf(pathBuf_, sizeof(pathBuf_), cwd_);
                    selected_.clear();
                    refresh();
                    break;
                }
            }
        }
        ImGui::EndChild();

        if (!error_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", error_.c_str());
        } else {
            ImGui::TextDisabled("double-click a folder to enter it");
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("hidden", &showHidden_)) refresh();

        const std::string chosen =
            selected_.empty() ? cwd_ : (fs::path(cwd_) / selected_).string();
        if (ImGui::Button("Use this folder", ImVec2(150, 0))) {
            out = chosen;
            picked = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", chosen.c_str());
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 100);
        if (ImGui::Button("Cancel", ImVec2(90, 0))) {
            ImGui::CloseCurrentPopup();
            open_ = false;
        }
        ImGui::EndPopup();
    }

    if (picked || !stayOpen) open_ = false;
    return picked;
}
