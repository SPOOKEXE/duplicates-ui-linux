#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>

#include "hash_cache.h"
#include "session.h"
#include "ui.h"

namespace {

void glfwErrorCallback(int code, const char* desc) {
    std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
}

// GLFW delivers native drag-and-drop here. It fires on the main thread inside
// glfwPollEvents/glfwWaitEvents, so the paths can be stashed without a lock.
void dropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* state = static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (!state) return;
    for (int i = 0; i < count; ++i) state->droppedPaths.push_back(paths[i]);
}

}  // namespace

int main() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to initialise glfw\n");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1280, 860, "duplicates-ui", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no imgui.ini clutter; the layout is fixed anyway
    applyTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    AppState state;
    glfwSetWindowUserPointer(window, &state);
    glfwSetDropCallback(window, dropCallback);

    const std::string sessionFile = sessionPath();
    applySession(state, parseSession(readFileOrEmpty(sessionFile)));

    // Loaded once here rather than per scan: a million entries is worth reading
    // exactly one time, and it is shared by every scan the session runs.
    const std::string cacheFile = HashCache::defaultPath();
    state.cache.load(cacheFile);

    // Saving is driven by hashing the serialized session rather than by dirty
    // flags scattered through the UI: one place to get right, nothing to forget.
    size_t savedHash = 0;
    double nextSaveCheck = 0.0;

    while (!glfwWindowShouldClose(window)) {
        // Wait for input rather than spinning at vsync: the timeout only exists
        // so progress refreshes while a scan or an apply is running.
        const bool busy = state.engine.running() || state.actions.running();
        glfwWaitEventsTimeout(busy ? 0.1 : 0.5);

        handleDrops(state);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawUi(state);
        ImGui::Render();

        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        if (glfwGetTime() >= nextSaveCheck) {
            nextSaveCheck = glfwGetTime() + 2.0;
            saveSessionIfChanged(sessionFile, serializeSession(sessionFromState(state)), savedHash);
        }
    }

    // The scan owns threads that reference the cache, so it has to stop before
    // the cache is written or read from here.
    state.engine.cancel();
    state.actions.cancel();

    saveSessionIfChanged(sessionFile, serializeSession(sessionFromState(state)), savedHash);
    state.cache.save(cacheFile);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
