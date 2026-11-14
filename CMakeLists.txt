cmake_minimum_required(VERSION 3.16)
project(PoqEditorImgui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# imgui 源码目录（不随本仓库分发）。
# 配置时传入 -DIMGUI_DIR=<path>，目录应直接包含 imgui.h 和 backends/。
# ---------------------------------------------------------------------------
set(IMGUI_DIR "" CACHE PATH "Path to the Dear ImGui source directory")
if(NOT IMGUI_DIR OR NOT EXISTS "${IMGUI_DIR}/imgui.h")
    message(FATAL_ERROR
        "Dear ImGui was not found. Configure with -DIMGUI_DIR=<path-to-imgui>.")
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

# P3: PE VERSIONINFO(与 Help.cpp Ver: YYMMDDvN 对齐, 后缀 POQ_VER_BUILD 手动 bump)
set(POQ_VER_BUILD 1)
set(POQ_VER_Y 26)
set(POQ_VER_M 11)
set(POQ_VER_D 01)
set(POQ_VER_STRING "261101v1")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc"
    @ONLY)
# 版本号头文件: 后缀 N(POQ_VER_BUILD) 的唯一来源, Help.cpp 与 PE 资源共用
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h"
    @ONLY)

# 共享 Core + Native(OBJECT 库: 只编一次, 链接进各 exe, 不产出 PoqCore.lib)
set(POQ_CORE_SOURCES
    src/Core/ItemData.cpp
    src/Core/TextCodec.cpp
    src/Core/OffsetsConfig.cpp
    src/Core/Json.cpp
    src/Core/JsonTemplate.cpp
    src/Core/GameModder.cpp
    src/Core/GameModderInternal.cpp
    src/Core/GameModderInject.cpp
    src/Core/GameModderCapture.cpp
    src/Core/GameModderClose.cpp
    src/Core/GameModderFields.cpp
    src/Core/GameModderAttrs.cpp
    src/Core/GmCmdUtil.cpp
    src/Core/GmCmdRemote.cpp
    src/Core/GmCmdCapture.cpp
    src/Core/Characters.cpp
    src/Core/SummonSteam.cpp
    src/Core/SummonSteamInternal.cpp
    src/Core/SummonSteamPatch.cpp
    src/Core/SummonScratch.cpp
    src/Core/Summon130.cpp
    src/Core/GmCmd.cpp
    src/Core/GamFile.cpp
    src/Core/GamFileGInfo.cpp
    src/Core/GamFileAffix.cpp
    src/Core/GamFileAffixWrite.cpp
    src/Core/GamFileParse.cpp
    src/Core/GamFileSave.cpp
    src/Core/GamCatalog.cpp
    src/Native/ProcessMemory.cpp
    src/Native/ProcessMemoryDetail.cpp
    src/Native/ProcessMemoryEnum.cpp
)

add_library(PoqCore OBJECT ${POQ_CORE_SOURCES})
target_include_directories(PoqCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(PoqCore PRIVATE /W4 /permissive- /WX)
    target_compile_definitions(PoqCore PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

set(PROJECT_SOURCES
    src/main.cpp
    src/Platform/Win32D3dBackend.cpp
    src/UI/UiText.cpp
    src/UI/App.cpp
    src/UI/CharacterTabLogic.cpp
    src/UI/ItemTabLogic.cpp
    src/UI/ItemTabPanels.cpp
    src/UI/CharTabPanel.cpp
    src/UI/GamTabLogic.cpp
    src/UI/GamTabToolbar.cpp
    src/UI/GamTabTree.cpp
    src/UI/GamTabEdit.cpp
    src/UI/TabChar.cpp
    src/UI/SummonTabData.cpp
    src/UI/SummonTabLogic.cpp
    src/UI/SummonTabPanel.cpp
    src/UI/GmTabLogic.cpp
    src/UI/GmTabPanel.cpp
    src/UI/MainWindowTopBar.cpp
    src/UI/MainWindowAuxTabs.cpp
    src/UI/TabGam.cpp
    src/UI/H2TabLogic.cpp
    src/UI/H2TabPanel.cpp
    src/UI/Help.cpp
    src/UI/MainWindow.cpp
)

set(PROJECT_HEADERS
    src/UI/App.h
    src/UI/AppGamState.h
    src/UI/AppCharState.h
    src/UI/AppCfgState.h
    src/UI/AppItemState.h
    src/UI/AppProcessState.h
    src/UI/AppCommon.h
    src/UI/ApiChar.h
    src/UI/ApiSummon.h
    src/UI/ApiGm.h
    src/UI/ApiGam.h
    src/UI/ApiH2.h
    src/UI/ApiMain.h
    src/UI/UiText.h
    src/Core/ItemProperty.h
    src/Core/ItemData.h
  src/Core/TextCodec.h
    src/Core/OffsetsConfig.h
    src/Core/Json.h
    src/Core/JsonTemplate.h
    src/Core/GameModder.h
    src/Core/GameModderInternal.h
    src/Core/GmCmdInternal.h
    src/Core/Characters.h
    src/Core/SummonSteam.h
    src/Core/SummonSteamInternal.h
    src/Core/Summon130.h
    src/Core/GmCmd.h
    src/Core/GamFile.h
    src/Core/GamCatalog.h
    src/Native/ProcessMemory.h
    src/Native/ProcessMemoryDetail.h
    src/Platform/Win32D3dBackend.h
)

# WIN32 子系统:无控制台窗口(WinMain 入口)
add_executable(PoqEditorImgui WIN32 ${IMGUI_SOURCES} ${PROJECT_SOURCES} ${PROJECT_HEADERS}
    app.rc "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")

# 离线回归工具 AffixRt(不进主 exe): 词条数组往返回归, 见 docs/affix_regression.md
# 构建: cmake --build build --config Release --target AffixRt
add_executable(AffixRt EXCLUDE_FROM_ALL src/Tools/AffixRt.cpp)
target_link_libraries(AffixRt PRIVATE PoqCore)
set_target_properties(AffixRt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")

add_executable(JsonRt EXCLUDE_FROM_ALL src/Tools/JsonRt.cpp)
target_link_libraries(JsonRt PRIVATE PoqCore)
set_target_properties(JsonRt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tools")

target_include_directories(PoqEditorImgui PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated   # 生成的 Version.h(版本号唯一来源)
)

# DX11 / DXGI / d3dcompiler(Windows SDK)+ shell32(任务栏图标 API)
target_link_libraries(PoqEditorImgui PRIVATE
    PoqCore
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
    # P3 #19: /W4 已清零后启用 /WX, 新告警即失败
    target_compile_options(PoqEditorImgui PRIVATE /W4 /permissive- /WX
        $<$<CONFIG:Release>:/GL>)
    target_link_options(PoqEditorImgui PRIVATE
        $<$<CONFIG:Release>:/LTCG>)
    # C4996(_wfopen 等 CRT "不安全函数"提示)是 API 偏好而非缺陷, 项目按需选用 -> 关闭该类告警
    target_compile_definitions(PoqEditorImgui PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
