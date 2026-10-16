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
    src/UI/UiText.cpp
    src/UI/TabChar.cpp
    src/UI/TabSummon.cpp
    src/UI/TabGm.cpp
    src/UI/TabGam.cpp
    src/UI/TabH2.cpp
    src/UI/Help.cpp
    src/UI/MainWindow.cpp
    src/Core/ItemData.cpp
  src/Core/TextCodec.cpp
    src/Core/OffsetsConfig.cpp
    src/Core/Json.cpp
    src/Core/JsonTemplate.cpp
    src/Core/GameModder.cpp
    src/Core/Characters.cpp
    src/Core/SummonSteam.cpp
    src/Core/Summon130.cpp
    src/Core/GmCmd.cpp
    src/Core/GamFile.cpp
    src/Core/GamCatalog.cpp
    src/Native/ProcessMemory.cpp
)

set(PROJECT_HEADERS
    src/UI/App.h
    src/UI/UiText.h
    src/Core/ItemProperty.h
    src/Core/ItemData.h
  src/Core/TextCodec.h
    src/Core/OffsetsConfig.h
    src/Core/Json.h
    src/Core/JsonTemplate.h
    src/Core/GameModder.h
    src/Core/Characters.h
    src/Core/SummonSteam.h
    src/Core/Summon130.h
    src/Core/GmCmd.h
    src/Core/GamFile.h
    src/Core/GamCatalog.h
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

# 拷贝 data/*(无中生有物品目录 gam_catalog.bin)到输出目录的 data/ 子目录
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data")
    add_custom_command(TARGET PoqEditorImgui POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/data"
            "$<TARGET_FILE_DIR:PoqEditorImgui>/data"
        COMMENT "copy data (gam catalog) to output")
endif()

# 我们的 app.rc 已内嵌 requireAdministrator manifest;禁用链接器自动生成的
# 默认 manifest,避免 CVT1100 资源重复(类型 MANIFEST 名称 1)。
if(MSVC)
    target_link_options(PoqEditorImgui PRIVATE /MANIFEST:NO)
    # 计划 P3(#19): 提高告警等级。/permissive- 收紧旧 MSVC 宽松行为。
    # 先只开 /W4 不加 /WX(告警不当错误), 便于逐步清理。
    target_compile_options(PoqEditorImgui PRIVATE /W4 /permissive-)
    # C4996(_wfopen 等 CRT "不安全函数"提示)是 API 偏好而非缺陷, 项目按需选用 -> 关闭该类告警
    target_compile_definitions(PoqEditorImgui PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
