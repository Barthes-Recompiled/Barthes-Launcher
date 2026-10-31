# barthes_launcher.exe: the Barthes Recompiled launcher for the Harry Potter Xbox ports (src/main.cpp).
# Windows only; built with MSYS2 UCRT64 (gcc, cmake, ninja), the same toolchain as the games:
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake_minimum_required(VERSION 3.20)
project(barthes_launcher C CXX)

set(BARTHES_VERSION "1.0.0" CACHE STRING "The launcher's version, shown in its colophon and its file properties")
string(REGEX MATCHALL "[0-9]+" _v "${BARTHES_VERSION}.0.0.0")
list(GET _v 0 V0)
list(GET _v 1 V1)
list(GET _v 2 V2)
set(ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")
set(MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/res/barthes.manifest")
configure_file(res/barthes.rc.in "${CMAKE_CURRENT_BINARY_DIR}/barthes.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/barthes.rc" PROPERTIES
    OBJECT_DEPENDS "${ICON_PATH};${MANIFEST_PATH}")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
add_executable(barthes_launcher WIN32
    src/main.cpp
    src/games.cpp
    src/net.cpp
    src/gamecheck.c  # which disc a folder or image holds; checks game\ against it (src/lists/*.inc)
    src/xiso.c       # unpacks a disc image into game\
    "${CMAKE_CURRENT_BINARY_DIR}/barthes.rc"
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(barthes_launcher PRIVATE ${IMGUI_DIR} src)
target_compile_definitions(barthes_launcher PRIVATE BARTHES_VERSION="${BARTHES_VERSION}" UNICODE _UNICODE
    _WIN32_WINNT=0x0A00 WINVER=0x0A00)
set_target_properties(barthes_launcher PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(barthes_launcher PRIVATE -O2)
target_link_options(barthes_launcher PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
target_link_libraries(barthes_launcher PRIVATE d3d11 dxgi d3dcompiler ole32 shell32 uuid imm32 dwmapi gdi32
    winhttp bcrypt version)
