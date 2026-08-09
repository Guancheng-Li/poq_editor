cmake_minimum_required(VERSION 3.16)
project(PoqEditorImgui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# imgui 源码路径(普通版 1.92.1)
#   - 用户机器默认: C:/env/imgui-1.92.1
#   - 可用 -DIMGUI_DIR=... 覆盖,指向包含 imgui.h 的目录
# ---------------------------------------------------------------------------
if(NOT DEFINED IMGUI_DIR)
    if(EXISTS "C:/env/imgui-1.92.1/imgui.h")
        set(IMGUI_DIR "C:/env/imgui-1.92.1")
    elseif(EXISTS "C:/env/imgui-docking/imgui.h")
        set(IMGUI_DIR "C:/env/imgui-docking")
    else()
        message(FATAL_ERROR "imgui not found. Use -DIMGUI_DIR=...")
    endif()
endif()
message(STATUS "Using imgui at: ${IMGUI_DIR}")

# ---------------------------------------------------------------------------
# MSVC /MT 静态运行时:单文件 exe,零 VCRUNTIME DLL 依赖
# ---------------------------------------------------------------------------
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Unicode + Win7+ API(SetCurrentProcessExplicitAppUserModelID 等)
# NOMINMAX:避免 windows.h 的 min/max 宏与 std::min/std::max 冲突
add_compile_definitions(UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00 NOMINMAX)
# 源文件全部 UTF-8
add_compile_options(/utf-8)

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

set(PROJECT_SOURCES
    src/main.cpp
    src/Core/ItemData.cpp
    src/Core/OffsetsConfig.cpp
    src/Core/Json.cpp
    src/Core/JsonTemplate.cpp
    src/Core/GameModder.cpp
    src/Native/ProcessMemory.cpp
)

set(PROJECT_HEADERS
    src/Core/ItemProperty.h
    src/Core/ItemData.h
    src/Core/OffsetsConfig.h
    src/Core/Json.h
    src/Core/JsonTemplate.h
    src/Core/GameModder.h
    src/Native/ProcessMemory.h
)

# WIN32 子系统:无控制台窗口(WinMain 入口)
add_executable(PoqEditorImgui WIN32 ${IMGUI_SOURCES} ${PROJECT_SOURCES} ${PROJECT_HEADERS} app.rc)

target_include_directories(PoqEditorImgui PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# DX11 / DXGI / d3dcompiler(Windows SDK)+ shell32(任务栏图标 API)
target_link_libraries(PoqEditorImgui PRIVATE
    d3d11
    dxgi
    d3dcompiler
    shell32
    user32
    gdi32
)

set_target_properties(PoqEditorImgui PROPERTIES
    WIN32_EXECUTABLE ON
    OUTPUT_NAME "PoqEditor"
)

# 我们的 app.rc 已内嵌 requireAdministrator manifest;禁用链接器自动生成的
# 默认 manifest,避免 CVT1100 资源重复(类型 MANIFEST 名称 1)。
if(MSVC)
    target_link_options(PoqEditorImgui PRIVATE /MANIFEST:NO)
endif()
