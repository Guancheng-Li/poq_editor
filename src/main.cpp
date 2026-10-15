// 秦殇物品修改器 (PoqEditor) —— imgui 单文件版。
//
// 对应 poqeditor_cpp(C++/Qt)的 100% 功能复刻,UI 改用 Dear ImGui(Win32 + DirectX11,
// WARP 软件渲染回退),默认蓝色主题 + 中文字体(运行时加载系统微软雅黑),零 DLL 依赖单文件 exe。
//
// 结构:
//   WinMain(入口,无控制台窗口) -> D3D11 初始化 -> imgui 初始化 -> 主循环
//   主循环每帧渲染:进程栏 / TabBar(物品修改、内存查看、偏移设置、gam修改,F12 切换隐藏) / 状态栏
//   App 结构持有全部状态;GameModder/ProcessMemory 等核心逻辑与 cpp 版完全一致。

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>    // SHBrowseForFolderW(目录选择)
#include <shobjidl.h>  // SetCurrentProcessExplicitAppUserModelID


#include <d3d11.h>
#include <tchar.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <functional>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "Core/Characters.h"
#include "Core/GamCatalog.h"
#include "Core/GamFile.h"
#include "Core/GameModder.h"
#include "Core/GmCmd.h"
#include "Core/ItemData.h"
#include "Core/ItemProperty.h"
#include "Core/Json.h"
#include "Core/JsonTemplate.h"
#include "Core/OffsetsConfig.h"
#include "Core/Summon130.h"
#include "Core/SummonSteam.h"
#include "Native/ProcessMemory.h"

#include "UI/App.h"

// imgui_impl_win32.h 用 #if 0 注释掉了这个声明(避免拖入 windows.h 依赖),
// 这里在全局作用域补上前置声明,供 poq::WndProc 调用。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace poq {

// ============================================================================
// DirectX 11 样板(抄自 imgui example_win32_directx11,含 WARP 软渲染回退)
// ============================================================================

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool g_SwapChainOccluded = false;
static UINT g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
bool CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)  // 无 GPU 驱动时回退 WARP 软件渲染
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                            createDeviceFlags, featureLevelArray, 2,
                                            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
                                            &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    return CreateRenderTarget();  // 计划 P2(#14): RTV 建不起来就算初始化失败
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dDeviceContext) {
        g_pd3dDeviceContext->Release();
        g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

// 计划 P2(#14): 返回 bool + 逐步检查 HRESULT; 失败时 g_mainRenderTargetView 保持 nullptr,
// 调用方据此跳过本帧渲染(而不是拿无效 RTV 去 OMSetRenderTargets)。
bool CreateRenderTarget() {
    if (!g_pSwapChain || !g_pd3dDevice) return false;
    CleanupRenderTarget();  // 重建前先释放旧的, 避免失败时残留失效 RTV
    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(hr) || !pBackBuffer) {
        if (pBackBuffer) pBackBuffer->Release();
        return false;
    }
    hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) {
        g_mainRenderTargetView = nullptr;
        return false;
    }
    return true;
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;  // 禁用 ALT 菜单
            break;
        case WM_CLOSE:
            // 本次打开生成过备份 -> 先问用户要不要清理(可"取消退出"), 不自作主张
            if (!g_open_backups.empty() && !g_exit_asked) {
                g_exit_asked = true;
                g_exit_ask_pending = true;  // 交给主循环弹框
                return 0;                   // 先不关窗口
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

ImFont* LoadChineseFont(float dpi_scale, bool bold) {
    ImGuiIO& io = ImGui::GetIO();
    static const char* kRegular[] = {
        "C:/Windows/Fonts/msyh.ttc",    // 微软雅黑
        "C:/Windows/Fonts/simhei.ttf",  // 黑体
        "C:/Windows/Fonts/simsun.ttc",  // 宋体
        "C:/Windows/Fonts/simsunb.ttf",
    };
    static const char* kBold[] = {
        "C:/Windows/Fonts/msyhbd.ttc",  // 微软雅黑 粗体
        "C:/Windows/Fonts/msyh.ttc",    "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsunb.ttf", "C:/Windows/Fonts/simsun.ttc",
    };
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    const float size = 16.0f * dpi_scale;
    const char* const* cands = bold ? kBold : kRegular;
    const int count = bold ? static_cast<int>(sizeof(kBold) / sizeof(kBold[0]))
                           : static_cast<int>(sizeof(kRegular) / sizeof(kRegular[0]));
    for (int i = 0; i < count; ++i) {
        ImFontConfig cfg;
        cfg.FontNo = 0;  // ttc 集合内第 0 个
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        ImFont* f = io.Fonts->AddFontFromFileTTF(cands[i], size, &cfg, ranges);
        if (f) return f;
    }
    return nullptr;
}

// ============================================================================
// 入口(GUI 子系统,无控制台窗口)
// ============================================================================

int RunApp(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 任务栏使用自定义图标(与 exe 图标一致)
    ::SetCurrentProcessExplicitAppUserModelID(L"PoqEditor");

    // DPI 感知 + 主显示器缩放
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    // 创建窗口(图标来自 .rc 内嵌资源)
    HICON hIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc = {sizeof(wc),        CS_CLASSDC, WndProc, 0L,      0L,
                      hInstance,         hIcon,      nullptr, nullptr, nullptr,
                      L"PoqEditorImgui", nullptr};
    wc.hIconSm = hIcon;
    // 计划 P2(#15): 检查 Win32 初始化返回值(原来全部忽略, 失败会带着无效句柄往下跑)
    if (!::RegisterClassExW(&wc)) {
        ::MessageBoxW(nullptr, L"RegisterClassExW 失败。", L"PoqEditor", MB_ICONERROR);
        return 1;
    }
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"秦殇修改器", WS_OVERLAPPEDWINDOW, 100, 100,
                                (int)(1100 * main_scale), (int)(720 * main_scale), nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ::MessageBoxW(nullptr, L"CreateWindowW 失败。", L"PoqEditor", MB_ICONERROR);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ::MessageBoxW(nullptr, L"DirectX 11 初始化失败(硬件/WARP 都不可用)。", L"PoqEditor",
                      MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // imgui 初始化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyBlueTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    // 计划 P2(#15): backend 初始化失败要逆序清理已成功的阶段, 而不是继续跑一个半残的渲染循环
    if (!ImGui_ImplWin32_Init(hwnd)) {
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ::MessageBoxW(nullptr, L"ImGui Win32 backend 初始化失败。", L"PoqEditor", MB_ICONERROR);
        return 1;
    }
    if (!ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ::MessageBoxW(nullptr, L"ImGui DX11 backend 初始化失败。", L"PoqEditor", MB_ICONERROR);
        return 1;
    }

    // 中文字体(regular + bold)
    ImFont* font_cn = LoadChineseFont(main_scale, false);
    ImFont* font_bold = LoadChineseFont(main_scale, true);
    if (!font_cn) font_cn = io.Fonts->AddFontDefault();
    io.FontDefault = font_cn;
    // bold 加载失败则回退 regular
    if (font_bold == nullptr) font_bold = font_cn;

    App app;
    app.gam_last_dir = GamLoadPrefs();
    // 常用命令下拉(工具目录 gm_cmd.txt,随工具走);格式错误行 -> 弹窗提示(有效行仍保留)
    int gm_cmd_err = LoadGmCmdList(app, /*keep_previous_on_error=*/false);
    if (gm_cmd_err > 0)
        MsgBox("读取配置",
               "常用命令文件 gm_cmd.txt 有 " + std::to_string(gm_cmd_err) +
                   " 行格式错误，已忽略这些行。",
               MB_ICONWARNING);

    const ImVec4 clear_color = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);

    // 主循环
    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            HRESULT rs = g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                                     DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            if (FAILED(rs) || !CreateRenderTarget()) {
                // 重建失败: 本帧不渲染(渲染循环会检查 g_mainRenderTargetView)
                continue;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // F12 切换调试 tab(内存查看 / 偏移设置 / gam修改)
        if (ImGui::IsKeyPressed(ImGuiKey_F12)) app.hidden = !app.hidden;

        // 无敌/一击必杀定时写回(每 500ms,对所有队员)
        TickLock(app, io.DeltaTime);
        // GM 状态周期刷新(每 2s,仅 GM tab 可见时)
        TickGm(app, io.DeltaTime);

        RenderMainWindow(app);
        RenderWelcomeDialog(app);
        PollExitAsk();  // 收到关闭请求且本次生成过备份: 弹"是否清理备份"询问
        RenderMsgQueue();

        // 渲染(RTV 不可用时跳过本帧: 计划 P2 #14 —— 拿 nullptr 当 RTV 会直接崩)
        ImGui::Render();
        if (!g_mainRenderTargetView) continue;
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                                   (const float*)&clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // 关闭:还原自注入的字节并释放进程句柄。
    // 退出路径不能抛异常弹窗, 失败只落到日志(计划 P0 的不变量已在 Close() 内保证)。
    {
        auto cr = app.modder.CloseNoThrowResult();
        if (!cr.hook_restored || !cr.cave_freed) {
            ::OutputDebugStringA(("PoqEditor: 退出时注入清理失败: " + cr.error).c_str());
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
}  // namespace poq

// 全局 WinMain 入口(GUI 子系统,无控制台窗口):桥接到 poq::RunApp。
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return poq::RunApp(hInstance, hPrevInstance, lpCmdLine, nCmdShow);
}
