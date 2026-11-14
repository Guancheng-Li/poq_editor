#pragma once
// Win32 窗口 + DirectX11 + ImGui 后端生命周期(从 main.cpp 拆出)。

#include <windows.h>

struct ImFont;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;

namespace poq::platform {

bool create_device_d3d(HWND hwnd);
void cleanup_device_d3d();
bool create_render_target();
void cleanup_render_target();

/// 处理 WM_SIZE 缓冲; 若应跳过本帧渲染返回 false。
bool prepare_frame_after_resize();

bool present_frame(const float clear_color_rgba[4]);
bool swap_chain_occluded();

ImFont* load_chinese_font(float dpi_scale, bool bold);

LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM w_param, LPARAM l_param);

ID3D11Device* d3d_device();
ID3D11DeviceContext* d3d_device_context();
IDXGISwapChain* swap_chain();
ID3D11RenderTargetView* main_render_target();

}  // namespace poq::platform
