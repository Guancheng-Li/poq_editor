#pragma once
// 秦殇物品修改器 —— 文本/工具/基础设施声明(#18 自 src/main.cpp 拆分)。
// 实现在 src/UI/UiText.cpp。

#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace poq {

std::string trim(const std::string& s);
std::string to_lower(const std::string& s);
std::string wide_to_utf8(const std::wstring& w);
std::wstring utf8_to_wide(const std::string& s);
std::wstring app_dir_w();

void track_backup(const std::wstring& src, const std::wstring& bak);

// 主循环里调用: 需要时弹出退出询问。
void poll_exit_ask();
// 退出时调用: 备份与原文件一致(即打开后没改过)的全部删除, 返回删除个数。
int cleanup_unchanged_backups();

// imgui 消息弹窗队列的消息体(替代系统 MessageBox:入队后主循环逐帧弹 imgui modal,
// 样式与启动免责声明弹窗一致)。单文件单实例,全局队列保持 msg_box 调用点签名不变。
struct MessageBoxMsg {
  std::string title;
  std::string text;
  int type = MB_ICONINFORMATION;
  std::function<void(int)> on_answer;  // 非空 = 三选一询问(0=全部复制 1=只复制属性 2=取消复制)
  std::string b0, b1, b2;              // 三选一按钮文字(空 = 用默认那三个文字)
};

void msg_box(const std::string& title, const std::string& text, UINT type);
// 三选一询问框: 回调 0=全部复制 1=只复制属性(不动名字/别名/类型) 2=取消复制。
void msg_box_ask(const std::string& title, const std::string& text, std::function<void(int)> on);
// 三选一询问框(自定义按钮文字)。
void msg_box_ask_ex(const std::string& title, const std::string& text, const char* b0,
                    const char* b1, const char* b2, std::function<void(int)> on);

struct App;  // 前置声明(App.h 依赖本头, 不能反向 include)

// P5 收敛: 三处"名字/别名/类型要不要一起"询问框(复制上一件/导入模板/GAM 粘贴)
// 的公共管线。body = 调用方拼好的来源描述("来源物品:"+"  名字: x"… 各处文案不同);
// tail = 询问尾句。按钮2=取消: 记 cancel_msg 到状态栏, 什么都不做;
// 按钮0=一起(with_ident=true), 按钮1=只属性, 结果回给 on_choice。
void ask_include_ident(App& app, const char* title, const std::string& body, const char* tail,
                       const char* cancel_msg,
                       const std::function<void(bool with_ident)>& on_choice);

std::string open_file_dialog(const std::wstring& dir, const wchar_t* filter);
std::string save_file_dialog(const std::wstring& dir, const wchar_t* filter);

std::string hex8(uintptr_t a);
std::string hex_str(uint32_t v);
bool is_hex(char c);
// 非调试模式下去掉消息里的内存地址(0x...),只保留动作结果文本。
std::string sanitize(const std::string& msg, bool debug);
// 浮点值显示:整数去 .0,否则原样。
std::string format_value(float v);
std::string format_hex_dump(uintptr_t base_addr, const std::vector<unsigned char>& data);
std::vector<unsigned char> parse_hex_dump(const std::string& text);

}  // namespace poq
