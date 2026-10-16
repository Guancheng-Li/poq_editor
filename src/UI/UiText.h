#pragma once
// 秦殇物品修改器 —— 文本/工具/基础设施声明(#18 自 src/main.cpp 拆分)。
// 实现在 src/UI/UiText.cpp。

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace poq {

std::string Trim(const std::string& s);
std::string ToLower(const std::string& s);
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::wstring AppDirW();

void TrackBackup(const std::wstring& src, const std::wstring& bak);

// 主循环里调用: 需要时弹出退出询问。
void PollExitAsk();
// 退出时调用: 备份与原文件一致(即打开后没改过)的全部删除, 返回删除个数。
int CleanupUnchangedBackups();

// imgui 消息弹窗队列的消息体(替代系统 MessageBox:入队后主循环逐帧弹 imgui modal,
// 样式与启动免责声明弹窗一致)。单文件单实例,全局队列保持 MsgBox 调用点签名不变。
struct MessageBoxMsg {
    std::string title;
    std::string text;
    int type = MB_ICONINFORMATION;
    std::function<void(int)> on_answer;  // 非空 = 三选一询问(0=全部复制 1=只复制属性 2=取消复制)
    std::string b0, b1, b2;              // 三选一按钮文字(空 = 用默认那三个文字)
};

void MsgBox(const std::string& title, const std::string& text, UINT type);
// 三选一询问框: 回调 0=全部复制 1=只复制属性(不动名字/别名/类型) 2=取消复制。
void MsgBoxAsk(const std::string& title, const std::string& text, std::function<void(int)> on);
// 三选一询问框(自定义按钮文字)。
void MsgBoxAskEx(const std::string& title, const std::string& text, const char* b0, const char* b1,
                 const char* b2, std::function<void(int)> on);

struct App;  // 前置声明(App.h 依赖本头, 不能反向 include)

// P5 收敛: 三处"名字/别名/类型要不要一起"询问框(复制上一件/导入模板/GAM 粘贴)
// 的公共管线。body = 调用方拼好的来源描述("来源物品:"+"  名字: x"… 各处文案不同);
// tail = 询问尾句。按钮2=取消: 记 cancel_msg 到状态栏, 什么都不做;
// 按钮0=一起(with_ident=true), 按钮1=只属性, 结果回给 on_choice。
void AskIncludeIdent(App& app, const char* title, const std::string& body, const char* tail,
                     const char* cancel_msg,
                     const std::function<void(bool with_ident)>& on_choice);

std::string OpenFileDialog(const std::wstring& dir, const wchar_t* filter);
std::string SaveFileDialog(const std::wstring& dir, const wchar_t* filter);

std::string Hex8(uintptr_t a);
std::string HexStr(uint32_t v);
bool IsHex(char c);
// 非调试模式下去掉消息里的内存地址(0x...),只保留动作结果文本。
std::string Sanitize(const std::string& msg, bool debug);
// 浮点值显示:整数去 .0,否则原样。
std::string FormatValue(float v);
std::string FormatHexDump(uintptr_t base_addr, const std::vector<unsigned char>& data);
std::vector<unsigned char> ParseHexDump(const std::string& text);

}  // namespace poq
