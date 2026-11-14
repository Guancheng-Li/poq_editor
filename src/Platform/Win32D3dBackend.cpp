#include "Platform/Win32D3dBackend.h"

#include <d3d11.h>
#include <tchar.h>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "UI/AppCommon.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace poq::platform {

namespace {

ID3D11Device* g_pd3d_device = nullptr;
ID3D11DeviceContext* g_pd3d_device_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
bool g_swap_chain_occluded = false;
UINT g_resize_width = 0;
UINT g_resize_height = 0;
ID3D11RenderTargetView* g_main_render_target_view = nullptr;

}  // namespace

ID3D11Device* d3d_device() {
  return g_pd3d_device;
}
ID3D11DeviceContext* d3d_device_context() {
  return g_pd3d_device_context;
}
IDXGISwapChain* swap_chain() {
  return g_swap_chain;
}
ID3D11RenderTargetView* main_render_target() {
  return g_main_render_target_view;
}
bool swap_chain_occluded() {
  return g_swap_chain_occluded;
}

bool create_device_d3d(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  D3D_FEATURE_LEVEL feature_level;
  const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  HRESULT res = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
      &g_swap_chain, &g_pd3d_device, &feature_level, &g_pd3d_device_context);
  if (res == DXGI_ERROR_UNSUPPORTED)
    res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                        D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_pd3d_device,
                                        &feature_level, &g_pd3d_device_context);
  if (res != S_OK) return false;
  return create_render_target();
}

void cleanup_device_d3d() {
  cleanup_render_target();
  if (g_swap_chain) {
    g_swap_chain->Release();
    g_swap_chain = nullptr;
  }
  if (g_pd3d_device_context) {
    g_pd3d_device_context->Release();
    g_pd3d_device_context = nullptr;
  }
  if (g_pd3d_device) {
    g_pd3d_device->Release();
    g_pd3d_device = nullptr;
  }
}

bool create_render_target() {
  if (!g_swap_chain || !g_pd3d_device) return false;
  cleanup_render_target();
  ID3D11Texture2D* back = nullptr;
  HRESULT hr = g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
  if (FAILED(hr) || !back) {
    if (back) back->Release();
    return false;
  }
  hr = g_pd3d_device->CreateRenderTargetView(back, nullptr, &g_main_render_target_view);
  back->Release();
  if (FAILED(hr)) {
    g_main_render_target_view = nullptr;
    return false;
  }
  return true;
}

void cleanup_render_target() {
  if (g_main_render_target_view) {
    g_main_render_target_view->Release();
    g_main_render_target_view = nullptr;
  }
}

LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, w_param, l_param)) return true;
  switch (msg) {
    case WM_SIZE:
      if (w_param == SIZE_MINIMIZED) return 0;
      g_resize_width = static_cast<UINT>(LOWORD(l_param));
      g_resize_height = static_cast<UINT>(HIWORD(l_param));
      return 0;
    case WM_SYSCOMMAND:
      if ((w_param & 0xfff0) == SC_KEYMENU) return 0;
      break;
    case WM_CLOSE:
      if (!poq::g_open_backups.empty() && !poq::g_exit_asked) {
        poq::g_exit_asked = true;
        poq::g_exit_ask_pending = true;
        return 0;
      }
      break;
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, w_param, l_param);
}

ImFont* load_chinese_font(float dpi_scale, bool bold) {
  ImGuiIO& io = ImGui::GetIO();
  static const char* k_regular[] = {"C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
                                    "C:/Windows/Fonts/simsun.ttc", "C:/Windows/Fonts/simsunb.ttf"};
  static const char* k_bold[] = {"C:/Windows/Fonts/msyhbd.ttc", "C:/Windows/Fonts/msyh.ttc",
                                 "C:/Windows/Fonts/simhei.ttf", "C:/Windows/Fonts/simsunb.ttf",
                                 "C:/Windows/Fonts/simsun.ttc"};
  const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
  const float size = 16.0f * dpi_scale;
  const char* const* cands = bold ? k_bold : k_regular;
  const int count =
      bold ? static_cast<int>(sizeof(k_bold) / sizeof(k_bold[0]))
           : static_cast<int>(sizeof(k_regular) / sizeof(k_regular[0]));
  for (int i = 0; i < count; ++i) {
    ImFontConfig cfg;
    cfg.FontNo = 0;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    ImFont* f = io.Fonts->AddFontFromFileTTF(cands[i], size, &cfg, ranges);
    if (f) return f;
  }
  return nullptr;
}

bool prepare_frame_after_resize() {
  if (g_swap_chain_occluded &&
      g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
    ::Sleep(10);
    return false;
  }
  g_swap_chain_occluded = false;
  if (g_resize_width != 0 && g_resize_height != 0) {
    cleanup_render_target();
    HRESULT rs =
        g_swap_chain->ResizeBuffers(0, g_resize_width, g_resize_height, DXGI_FORMAT_UNKNOWN, 0);
    g_resize_width = g_resize_height = 0;
    if (FAILED(rs) || !create_render_target()) return false;
  }
  return true;
}

bool present_frame(const float clear_color_rgba[4]) {
  if (!g_main_render_target_view) return false;
  g_pd3d_device_context->OMSetRenderTargets(1, &g_main_render_target_view, nullptr);
  g_pd3d_device_context->ClearRenderTargetView(g_main_render_target_view, clear_color_rgba);
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  HRESULT hr = g_swap_chain->Present(1, 0);
  g_swap_chain_occluded = (hr == DXGI_STATUS_OCCLUDED);
  return true;
}

}  // namespace poq::platform
