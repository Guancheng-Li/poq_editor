// 秦殇物品修改器 (PoqEditor) —— 入口与主循环; 平台层见 Platform/Win32D3dBackend。

#include <windows.h>
#include <shobjidl.h>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "Core/GameModder.h"
#include "Platform/Win32D3dBackend.h"
#include "UI/App.h"
#include "UI/UiText.h"

namespace poq {

int run_app(HINSTANCE h_instance, HINSTANCE, LPSTR, int) {
  ::SetCurrentProcessExplicitAppUserModelID(L"PoqEditor");

  ImGui_ImplWin32_EnableDpiAwareness();
  float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
      ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

  HICON h_icon = ::LoadIconW(h_instance, MAKEINTRESOURCEW(1));
  WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, platform::wnd_proc, 0L, 0L, h_instance, h_icon,
                    nullptr, nullptr, nullptr, L"PoqEditorImgui", nullptr};
  wc.hIconSm = h_icon;
  if (!::RegisterClassExW(&wc)) {
    ::MessageBoxW(nullptr, L"RegisterClassExW 失败。", L"PoqEditor", MB_ICONERROR);
    return 1;
  }
  HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"秦殇修改器", WS_OVERLAPPEDWINDOW, 100, 100,
                              static_cast<int>(1100 * main_scale), static_cast<int>(720 * main_scale),
                              nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::MessageBoxW(nullptr, L"CreateWindowW 失败。", L"PoqEditor", MB_ICONERROR);
    return 1;
  }

  if (!platform::create_device_d3d(hwnd)) {
    platform::cleanup_device_d3d();
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::MessageBoxW(nullptr, L"DirectX 11 初始化失败(硬件/WARP 都不可用)。", L"PoqEditor",
                  MB_ICONERROR);
    return 1;
  }

  ::ShowWindow(hwnd, SW_SHOWDEFAULT);
  ::UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  apply_blue_theme();
  ImGuiStyle& style = ImGui::GetStyle();
  style.ScaleAllSizes(main_scale);
  style.FontScaleDpi = main_scale;

  if (!ImGui_ImplWin32_Init(hwnd)) {
    ImGui::DestroyContext();
    platform::cleanup_device_d3d();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::MessageBoxW(nullptr, L"ImGui Win32 backend 初始化失败。", L"PoqEditor", MB_ICONERROR);
    return 1;
  }
  if (!ImGui_ImplDX11_Init(platform::d3d_device(), platform::d3d_device_context())) {
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    platform::cleanup_device_d3d();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ::MessageBoxW(nullptr, L"ImGui DX11 backend 初始化失败。", L"PoqEditor", MB_ICONERROR);
    return 1;
  }

  ImFont* font_cn = platform::load_chinese_font(main_scale, false);
  ImFont* font_bold = platform::load_chinese_font(main_scale, true);
  if (!font_cn) font_cn = io.Fonts->AddFontDefault();
  io.FontDefault = font_cn;
  if (font_bold == nullptr) font_bold = font_cn;

  App app;
  int gm_cmd_err = load_gm_cmd_list(app, false);
  if (gm_cmd_err > 0)
    msg_box("读取配置",
            "常用命令文件 gm_cmd.txt 有 " + std::to_string(gm_cmd_err) + " 行格式错误，已忽略这些行。",
            MB_ICONWARNING);

  const float clear_color[4] = {0.12f, 0.14f, 0.18f, 1.00f};

  bool done = false;
  while (!done) {
    MSG msg;
    while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;

    if (!platform::prepare_frame_after_resize()) continue;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (ImGui::IsKeyPressed(ImGuiKey_F12)) app.hidden = !app.hidden;

    tick_lock(app, io.DeltaTime);
    tick_item_read(app, io.DeltaTime);
    tick_gm(app, io.DeltaTime);
    tick_summon(app, io.DeltaTime);

    render_main_window(app);
    render_welcome_dialog(app);
    poll_exit_ask();
    render_msg_queue();

    ImGui::Render();
    platform::present_frame(clear_color);
  }

  {
    auto cr = app.modder().close_no_throw_result();
    if (!cr.hook_restored || !cr.cave_freed) {
      ::OutputDebugStringA(("PoqEditor: 退出时注入清理失败: " + cr.error).c_str());
    }
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();

  platform::cleanup_device_d3d();
  ::DestroyWindow(hwnd);
  ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
  return 0;
}

}  // namespace poq

int WINAPI WinMain(HINSTANCE h_instance, HINSTANCE h_prev_instance, LPSTR lp_cmd_line, int n_cmd_show) {
  return poq::run_app(h_instance, h_prev_instance, lp_cmd_line, n_cmd_show);
}
