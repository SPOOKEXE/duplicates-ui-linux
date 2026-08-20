cmake_minimum_required(VERSION 3.16)
project(duplicates-ui VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

include(FetchContent)

# GLFW: window + input + the native file-drop callback the input list hangs on.
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

# Dear ImGui ships no CMakeLists, so we fetch the sources and build our own target below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9
    GIT_SHALLOW    TRUE)

FetchContent_MakeAvailable(glfw imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# Everything that is not drawing lives in one library, so the unit tests can
# link the real scanning and action code rather than a copy of it.
add_library(dupcore STATIC
    src/util.cpp
    src/log.cpp
    src/pipeline.cpp
    src/hash.cpp
    src/hash_cache.cpp
    src/scanner.cpp
    src/cascade.cpp
    src/scan_engine.cpp
    src/groups.cpp
    src/actions.cpp
    src/runs.cpp
    src/session.cpp)
target_include_directories(dupcore PUBLIC src)
target_link_libraries(dupcore PUBLIC Threads::Threads)
target_compile_options(dupcore PRIVATE -Wall -Wextra)

add_executable(duplicates-ui
    src/main.cpp
    src/ui.cpp
    src/ui_inputs.cpp
    src/ui_results.cpp
    src/ui_runs.cpp
    src/dir_browser.cpp)
target_link_libraries(duplicates-ui PRIVATE dupcore imgui)
target_compile_options(duplicates-ui PRIVATE -Wall -Wextra)
# So a built binary can say what it is. A release artifact that cannot be asked
# its version is a release artifact you cannot tell apart from any other.
target_compile_definitions(duplicates-ui PRIVATE
    DUPLICATES_UI_VERSION="${PROJECT_VERSION}")

enable_testing()
add_executable(unit-tests tests/unit_tests.cpp)
target_link_libraries(unit-tests PRIVATE dupcore)
target_compile_options(unit-tests PRIVATE -Wall -Wextra)
add_test(NAME unit COMMAND unit-tests)

# Release builds fold libstdc++ and libgcc into the binary. GLFW is already a
# static library and it dlopens the X11 libraries at runtime, so what remains is
# glibc plus the system's OpenGL, which is exactly what should come from the host.
option(DUPLICATES_UI_PORTABLE "Statically link libstdc++ and libgcc for distribution" OFF)
if(DUPLICATES_UI_PORTABLE)
    target_link_options(duplicates-ui PRIVATE -static-libstdc++ -static-libgcc)
endif()

install(TARGETS duplicates-ui RUNTIME DESTINATION bin)
