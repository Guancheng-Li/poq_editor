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
#include <shellapi.h>
#include <shobjidl.h>  // SetCurrentProcessExplicitAppUserModelID
#include <shlobj.h>    // SHBrowseForFolderW(目录选择)
#include <commdlg.h>

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

#include "Core/GameModder.h"
#include "Core/Characters.h"
#include "Core/ItemData.h"
#include "Core/ItemProperty.h"
#include "Core/Json.h"
#include "Core/JsonTemplate.h"
#include "Core/OffsetsConfig.h"
#include "Core/SummonSteam.h"
#include "Core/Summon130.h"
#include "Core/GmCmd.h"
#include "Core/GamFile.h"
#include "Core/GamCatalog.h"
#include "Native/ProcessMemory.h"

// imgui_impl_win32.h 用 #if 0 注释掉了这个声明(避免拖入 windows.h 依赖),
// 这里在全局作用域补上前置声明,供 poq::WndProc 调用。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace poq {

// ============================================================================
// 工具函数
// ============================================================================

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::wstring AppDirW() {
    wchar_t self[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring p(self);
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : p.substr(0, pos);
}

// imgui 消息弹窗队列(替代系统 MessageBox:入队后主循环逐帧弹 imgui modal,
// ---------------------------------------------------------------------------
// 打开时创建的备份登记: 退出时判定"这个文件打开后有没有被改过", 没改过就删掉它的备份,
// 避免反复打开堆一堆没用的 .bak; 改过的备份保留(那种是有用的)。
// 判定分两级(为了快):
//   1) 先比"大小 + 修改时间"(打开备份时记下的) —— 没变就说明没动过, 直接删, 不读文件
//      (NTFS 的修改时间是 100ns 精度, 同尺寸改写也必然改动它)
//   2) 变了才把两边算 MD5 做最终判定(改过但内容恰好一样的情况, 比如游戏重存了同内容)
// ---------------------------------------------------------------------------
struct OpenBackup {
    std::wstring src;    // 原文件
    std::wstring bak;    // 打开时生成的备份
    uint64_t size = 0;   // 备份时原文件的大小
    uint64_t mtime = 0;  // 备份时原文件的修改时间(FILETIME 原值)
};
std::vector<OpenBackup> g_open_backups;

static bool StatFile(const std::wstring& path, uint64_t& size, uint64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    mtime = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

void TrackBackup(const std::wstring& src, const std::wstring& bak) {
    OpenBackup rec;
    rec.src = src;
    rec.bak = bak;
    StatFile(src, rec.size, rec.mtime);   // 此刻原文件还没被改
    g_open_backups.push_back(std::move(rec));
}

// ---- MD5(RFC 1321)精简实现(只用于上面的比对, 不依赖外部程序) ----
namespace {
struct Md5Ctx {
    uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu, d = 0x10325476u;
    uint64_t len = 0;            // 已输入字节数
    unsigned char buf[64]{};
    size_t n = 0;                // buf 内已缓冲字节数
};
const uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
const int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
inline uint32_t Md5Rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void Md5Block(Md5Ctx& ctx, const unsigned char* p) {
    uint32_t m[16];
    std::memcpy(m, p, 64);   // x86 小端: 直接按 dword 解释, 省掉 64 次移位拼接
    uint32_t a = ctx.a, b = ctx.b, c = ctx.c, d = ctx.d;
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        f += a + kMd5K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += Md5Rotl(f, kMd5S[i]);
    }
    ctx.a += a;
    ctx.b += b;
    ctx.c += c;
    ctx.d += d;
}

void Md5Update(Md5Ctx& ctx, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    ctx.len += n;
    // 缓冲区空时直接从输入按 64B 整块处理, 不走 memcpy(否则大文件要逐块小拷贝, 很慢)
    if (ctx.n == 0) {
        while (n >= 64) {
            Md5Block(ctx, p);
            p += 64;
            n -= 64;
        }
    }
    while (n > 0) {
        size_t take = 64 - ctx.n;
        if (take > n) take = n;
        std::memcpy(ctx.buf + ctx.n, p, take);
        ctx.n += take;
        p += take;
        n -= take;
        if (ctx.n == 64) {
            Md5Block(ctx, ctx.buf);
            ctx.n = 0;
        }
    }
}

void Md5Final(Md5Ctx& ctx, unsigned char out[16]) {
    uint64_t bits = ctx.len * 8;
    unsigned char lb[8];
    // padding: 0x80 之后补零到 (56 mod 64) —— 一次喂完(原来逐字节调 Update 最多 63 次)
    size_t pad_len = (ctx.n < 56) ? (56 - ctx.n) : (120 - ctx.n);
    unsigned char pad[128];
    pad[0] = 0x80;
    if (pad_len > 1) std::memset(pad + 1, 0, pad_len - 1);
    Md5Update(ctx, pad, pad_len);
    for (int i = 0; i < 8; ++i) lb[i] = (unsigned char)((bits >> (8 * i)) & 0xFF);
    Md5Update(ctx, lb, 8);
    uint32_t v[4] = {ctx.a, ctx.b, ctx.c, ctx.d};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = (unsigned char)((v[i] >> (8 * j)) & 0xFF);
}

std::string Md5FileHex(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return "";
    Md5Ctx ctx;
    std::vector<unsigned char> buf(1u << 16);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) Md5Update(ctx, buf.data(), n);
    std::fclose(f);
    unsigned char dig[16];
    Md5Final(ctx, dig);
    static const char* kHex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        s.push_back(kHex[dig[i] >> 4]);
        s.push_back(kHex[dig[i] & 15]);
    }
    return s;
}
}  // namespace

bool g_exit_asked = false;        // 本次关闭是否已经问过
bool g_exit_ask_pending = false;  // 主循环需要弹出"退出前是否清理备份"的询问
// 这两个在本文件后面才定义(MsgBox 系列在下方), 先声明
void MsgBoxAskEx(const std::string& title, const std::string& text, const char* b0, const char* b1,
                 const char* b2, std::function<void(int)> on);
int CleanupUnchangedBackups();

// 主循环里调用: 需要时弹出退出询问。
// 是 = 按 MD5 校验清理没改过的备份并退出; 否 = 不清理直接退出; 取消 = 取消退出(留在程序里)。
void PollExitAsk() {
    if (!g_exit_ask_pending) return;
    g_exit_ask_pending = false;
    std::string q = "本次打开生成了 " + std::to_string(g_open_backups.size()) +
                    " 个备份文件。\n要用 MD5 校验清理掉其中\"没改动过\"的备份吗？\n"
                    "（校验要把文件读一遍，可能卡顿几秒；改过的备份会保留）";
    MsgBoxAskEx("退出前清理备份", q, "是", "否", "取消退出", [](int choice) {
        if (choice == 2) {
            g_exit_asked = false;  // 取消退出: 留在程序里, 下次关闭再问
            return;
        }
        if (choice == 0) CleanupUnchangedBackups();  // 是: 校验并清理
        ::PostQuitMessage(0);                        // 否: 不清理, 直接退出
    });
}

// 退出时调用: 备份与原文件一致(即打开后没改过)的全部删除, 返回删除个数。
int CleanupUnchangedBackups() {
    int removed = 0;
    for (const auto& rec : g_open_backups) {
        if (rec.src.empty() || rec.bak.empty()) continue;
        if (::GetFileAttributesW(rec.bak.c_str()) == INVALID_FILE_ATTRIBUTES) continue;  // 备份已不在
        uint64_t cur_size = 0, cur_mtime = 0;
        if (!StatFile(rec.src, cur_size, cur_mtime)) continue;  // 原文件没了: 留着备份
        if (cur_size == rec.size && cur_mtime == rec.mtime) {   // 快路径: 没动过, 不读文件
            if (::DeleteFileW(rec.bak.c_str())) ++removed;
            continue;
        }
        // 动过了: 再算 MD5(可能只是被重存了同样的内容)
        std::string hs = Md5FileHex(rec.src), hb = Md5FileHex(rec.bak);
        if (!hs.empty() && hs == hb && ::DeleteFileW(rec.bak.c_str())) ++removed;
    }
    g_open_backups.clear();
    return removed;
}

// 样式与启动免责声明弹窗一致)。单文件单实例,全局队列保持 MsgBox 调用点签名不变。
struct MessageBoxMsg {
    std::string title;
    std::string text;
    int type = MB_ICONINFORMATION;
    std::function<void(int)> on_answer;   // 非空 = 三选一询问(0=全部复制 1=只复制属性 2=取消复制)
    std::string b0, b1, b2;               // 三选一按钮文字(空 = 用默认那三个文字)
};
std::vector<MessageBoxMsg> g_msgbox_queue;

void MsgBox(const std::string& title, const std::string& text, UINT type) {
    g_msgbox_queue.push_back(MessageBoxMsg{title, text, static_cast<int>(type), {}});
}

// 三选一询问框: 回调 0=全部复制 1=只复制属性(不动名字/别名/类型) 2=取消复制。
void MsgBoxAsk(const std::string& title, const std::string& text, std::function<void(int)> on) {
    g_msgbox_queue.push_back(MessageBoxMsg{title, text, static_cast<int>(MB_ICONQUESTION), std::move(on), {}, {}, {}});
}

// 三选一询问框(自定义按钮文字)。
void MsgBoxAskEx(const std::string& title, const std::string& text, const char* b0, const char* b1,
                 const char* b2, std::function<void(int)> on) {
    MessageBoxMsg m;
    m.title = title;
    m.text = text;
    m.type = static_cast<int>(MB_ICONQUESTION);
    m.on_answer = std::move(on);
    m.b0 = b0 ? b0 : "";
    m.b1 = b1 ? b1 : "";
    m.b2 = b2 ? b2 : "";
    g_msgbox_queue.push_back(std::move(m));
}

std::string OpenFileDialog(const std::wstring& dir, const wchar_t* filter) {
    wchar_t buf[1024]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetOpenFileNameW(&ofn)) return WideToUtf8(buf);
    return "";
}

std::string SaveFileDialog(const std::wstring& dir, const wchar_t* filter) {
    wchar_t buf[1024]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (::GetSaveFileNameW(&ofn)) return WideToUtf8(buf);
    return "";
}

std::string Hex8(uintptr_t a) {
    char b[32];
    std::snprintf(b, sizeof(b), "%08llX", static_cast<unsigned long long>(a));
    return b;
}

std::string HexStr(uint32_t v) {
    char b[32];
    std::snprintf(b, sizeof(b), "0x%X", static_cast<unsigned>(v));
    return b;
}

bool IsHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 非调试模式下去掉消息里的内存地址(0x...),只保留动作结果文本。
std::string Sanitize(const std::string& msg, bool debug) {
    if (debug) return msg;
    std::string out;
    size_t i = 0;
    while (i < msg.size()) {
        // 去掉 "@ 0x12345678:"(含前导 @ 与随后的冒号)
        if (msg[i] == '@') {
            size_t j = i + 1;
            while (j < msg.size() && msg[j] == ' ') ++j;
            if (j + 1 < msg.size() && msg[j] == '0' && (msg[j + 1] == 'x' || msg[j + 1] == 'X')) {
                size_t k = j + 2;
                while (k < msg.size() && IsHex(msg[k])) ++k;
                if (k > j + 2) {
                    if (k < msg.size() && msg[k] == ':') ++k;
                    i = k;
                    continue;
                }
            }
        }
        // 去掉独立的 "0x1234"(>=4 位十六进制)
        if (msg[i] == '0' && i + 1 < msg.size() && (msg[i + 1] == 'x' || msg[i + 1] == 'X')) {
            size_t k = i + 2;
            while (k < msg.size() && IsHex(msg[k])) ++k;
            if (k - (i + 2) >= 4) {
                i = k;
                continue;
            }
        }
        out.push_back(msg[i]);
        ++i;
    }
    // 清理残留的多余空格
    std::string r;
    bool prev_space = false;
    for (char c : out) {
        if (c == ' ') {
            if (!prev_space) r += c;
            prev_space = true;
        } else {
            r += c;
            prev_space = false;
        }
    }
    return Trim(r);
}

// 浮点值显示:整数去 .0,否则原样。
std::string FormatValue(float v) {
    if (v == static_cast<float>(static_cast<int>(v))) return std::to_string(static_cast<int>(v));
    char b[64];
    std::snprintf(b, sizeof(b), "%g", static_cast<double>(v));
    return b;
}

std::string FormatHexDump(uintptr_t base_addr, const std::vector<unsigned char>& data) {
    std::string out;
    char line[64];
    for (size_t i = 0; i < data.size(); i += 16) {
        std::snprintf(line, sizeof(line), "%08llX: ", static_cast<unsigned long long>(base_addr + i));
        out += line;
        size_t n = std::min<size_t>(16, data.size() - i);
        for (size_t j = 0; j < n; ++j) {
            std::snprintf(line, sizeof(line), "%02X ", data[i + j]);
            out += line;
        }
        out += "\n";
    }
    return out;
}

std::vector<unsigned char> ParseHexDump(const std::string& text) {
    std::vector<unsigned char> bytes;
    std::string line;
    for (size_t i = 0; i <= text.size(); ++i) {
        char c = (i < text.size()) ? text[i] : '\n';
        if (c == '\r') continue;
        if (c != '\n') {
            line.push_back(c);
            continue;
        }
        // 处理一行
        std::string t = Trim(line);
        line.clear();
        if (t.empty()) continue;
        size_t colon = t.find(':');
        if (colon != std::string::npos) t = Trim(t.substr(colon + 1));
        // 按空白切 token
        size_t a = 0;
        while (a < t.size()) {
            while (a < t.size() && (t[a] == ' ' || t[a] == '\t')) ++a;
            size_t b = a;
            while (b < t.size() && t[b] != ' ' && t[b] != '\t') ++b;
            if (b > a) {
                std::string tok = t.substr(a, b - a);
                if (tok.size() == 2) {
                    char* end = nullptr;
                    unsigned long v = std::strtoul(tok.c_str(), &end, 16);
                    if (end == tok.c_str() + 2) bytes.push_back(static_cast<unsigned char>(v));
                }
            }
            a = b;
        }
    }
    return bytes;
}

// ============================================================================
// App 状态(对应 cpp MainWindow 的成员)
// ============================================================================

// 角色字段控件状态(当前选中角色一份;切换角色时从内存重填)。
struct CharWidget {
    int intval = 0;           // dword/word/enum
    int pair_lo = 0;          // pair 下限
    int pair_hi = 0;          // pair 上限
    char strbuf[128] = {0};   // string_ptr(UTF-8,足够 50 个汉字)
};

struct App {
    OffsetsConfig cfg;
    GameModder modder;
    std::vector<ProcessInfo> processes;
    std::vector<BasicFieldCfg> basic_fields;

    // ---- 基础属性控件状态(与 basic_fields 平行) ----
    struct BasicWidget {
        bool is_enum = false;
        int intval = 0;          // 数值字段
        int enum_val = 0;        // enum 字段当前值
        char strbuf[256] = {0};  // string_ptr(UTF-8,足够 50 个汉字=150 字节)
    };
    std::vector<BasicWidget> basic;

    // ---- 属性表格 ----
    std::vector<ItemProperty> attrs;
    std::vector<bool> attr_sel;

    // ---- 捕获后冻结 ----
    // 「读取属性」按下后:解冻->清槽->等一次捕获(手持物品被游戏读取)->读取->冻结。
    // 识别只发生一次;冻结后不再被悬停/场景读取顶掉。

    // ---- 上一件物品属性缓存 ----
    // 每次读取(或保存)成功时快照一份;"复制上一件物品"把它写到当前物品上。
    std::vector<ItemProperty> last_attrs;
    std::string last_name, last_alias;       // 上一件物品的名字/别名(复制时按需带过来)
    int last_type = -1;                      // 上一件物品的类型(同上)
    uintptr_t last_item_base = 0;

    // ---- 使用说明弹窗 ----
    // cur_tab:当前主 tab 下标(0=人物 1=道具 2=召唤 3=GM 4=H2,5/6=调试 tab);
    // help_jump/help_tab:点"使用说明"瞬间要跳到的 tab(仅弹窗打开后第一帧生效)。
    int cur_tab = 0;
    int help_tab = 0;
    bool help_jump = false;

    // ---- 进程栏 ----
    int proc_selected = -1;

    // ---- 内存查看 ----
    char raw_addr[64] = {0};
    char raw_size[16] = {0};
    char raw_hex[131072] = {0};

    // ---- 偏移设置 ----
    std::vector<std::string> mode_ids;
    int mode_selected = 0;
    char cfg_module[128] = {0};
    char cfg_offset[32] = {0};
    char cfg_aob[128] = {0};
    char cfg_weight_offset[32] = {0};
    char cfg_weight_max[32] = {0};
    char cfg_item_adjust[32] = {0};
    char cfg_head[32] = {0};
    char cfg_next[32] = {0};
    char cfg_id[32] = {0};
    char cfg_color[32] = {0};
    char cfg_precent[32] = {0};
    char cfg_value[32] = {0};

    // ---- 属性修改(角色)----
    std::map<std::string, uintptr_t> char_addrs;   // 姓名 -> 结构体地址
    std::vector<std::string> char_names;           // 下拉顺序(角色x-名字)
    int char_selected = 0;
    std::vector<CharWidget> char_widgets;          // 平行 CharFields,当前选中角色
    char money_buf[32] = {0};                      // 全局金钱输入
    bool ck_invincible = false;                    // 无敌模式
    bool ck_kill = false;                          // 一击必杀
    // 锁原值缓存:姓名 -> {label: 原值}(勾选时缓存,取消时恢复)
    std::map<std::string, std::map<std::string, CharValue>> def_cache;  // 防御/闪避
    std::map<std::string, std::map<std::string, CharValue>> hit_cache;  // 攻击/命中率
    float lock_accum = 0.0f;                       // 锁定时器累加(500ms 写回一次)
    std::string char_status;                       // 角色 tab 状态文本
    std::vector<std::string> char_slot_probe;      // F12 调试:槽位名字探测(不过滤)
    bool char_filter = true;                       // 角色属性校验过滤(白名单+等级),默认开启
    bool char_filter_warn = false;                 // 关闭过滤时的警告弹窗待显示

    // ---- 召唤修改(task7)----
    bool summon_active = false;       // 激活态(激活后按钮变"取消",刷新可点)
    std::string summon_log;           // 召唤工具输出(F12 调试模式显示)
    std::string summon_mode_cache;    // 上次渲染的版本,检测切换并重置激活态
    int sm_group = 0;                 // 召唤归属: 0=队友可控(默认) 1=盟友不可控
    int sm_kind = 0;                  // steam 三选一: 0=野兽 1=刺客 2=秦兵 (-1=全不选)
    int sm_mon = 0;                   // steam 野兽(0-9)
    int sm_assassin = 0;              // steam 刺客(0=弓箭 1=带刀)
    int sm_soldier = 0;               // steam 秦兵(0=随机 1-6=六大类)
    std::vector<int> sm_sub_items;    // 当前秦兵大类的子类(原索引列表,过滤旧重剑)
    int sm_sub = -1;                  // 选中子类(原索引)
    int sm130_kind = 0;               // 1.30 二选一: 0=野兽 1=随机秦兵 (-1=全不选)
    int sm130_mon = 0;                // 1.30 野兽(0-7,朱厌/善翼不显示)

    // ---- GM命令(task8)----
    bool gm_active = false;           // 当前 GM 是否已激活(is_gm_ready 决定)
    std::string gm_log;               // 工具日志(F12 调试模式显示)
    std::string gm_mode_cache;        // 上次渲染的版本,检测切换并重置激活态
    int gm_method = 0;                // 激活方式: 0=内存修改 1=R9注入(默认内存修改)
    char gm_cmd_buf[256] = {0};       // 命令输入框(UTF-8)
    bool ck_count = false;            // 执行次数
    char gm_count_buf[8] = {0};       // 执行次数输入(默认 "1")
    float gm_timer_accum = 0.0f;      // 2s 周期刷新 GM 状态(仅 GM tab 可见时跑)
    bool gm_visible = false;          // GM tab 当前是否显示
    std::vector<std::pair<std::string, std::string>> gm_cmd_list;  // 常用命令 (命令, 显示名)
    int gm_cmdlist_sel = 0;           // 常用命令下拉当前选中项
    // 命令历史(上下箭头切换):最新在末尾,最多 30 条;idx=-1 表示未在浏览。
    std::vector<std::string> gm_history;
    int gm_hist_idx = -1;
    std::string gm_hist_draft;        // 开始浏览瞬间保存的输入框原文(下翻回最末时还原)
    bool gm_cmdlist_focused = false;  // 常用命令列表当前是否持有焦点(持有时箭头留给列表)

    // ---- H2解密(task10)----
    std::wstring h2_path;             // 待转换文件(已有备份时等待用户选择)
    bool h2_decrypt = true;           // 待转换方向: true=解密 false=加密
    bool h2_wait_confirm = false;     // 已有备份,等待覆盖/跳过/取消选择
    std::wstring h2_dir;              // 待批量转换目录(等待确认)
    bool h2_dir_decrypt = true;       // 目录转换方向: true=解密 false=加密
    bool h2_wait_dir_confirm = false; // 目录转换确认弹窗待定

    // ---- GAM修改(task13)----
    poq::GamFile gam_file;                   // 已加载存档
    std::string gam_path;
    bool gam_loaded = false;
    int gam_sel_container = -1;              // 容器下标
    int gam_sel_item = -1;                   // 选中物品下标(gam_file.items)
    int gam_edit_loaded_item = -1;           // 词条编辑缓冲已加载的物品下标
    std::vector<uint8_t> gam_clip_record;    // 复制物品(整条记录字节)
    bool gam_has_record = false;
    std::map<std::string, int64_t> gam_clip_fields;  // 复制属性(固定字段快照)
    std::string gam_clip_ginfo;              // 复制属性(G_Info GBK 文本,空=无)
    std::string gam_clip_name, gam_clip_alias;  // 复制属性(名字 UTF-8 / 别名)
    bool gam_has_attrs = false;
    poq::GamCatalog gam_catalog;             // 无中生有数据源(全装备目录)
    bool gam_cat_loaded = false;
    int gam_sel_cat = 0;                     // 创建物品: 类别
    int gam_sel_cat_item = 0;                // 创建物品: 条目
    std::vector<std::pair<int, std::vector<uint8_t>>> gam_added;  // 保存时插入(容器,记录)
    std::string gam_last_dir;               // 上次打开 gam 的目录(记忆)
    poq::SceneCatalog gam_scene_cat;        // 场景全名目录(容器别名->完整场景路径)
    bool gam_scene_loaded = false;
    // GAM 树分级缓存(P1): 结构变化时重建, 不再每帧构建
    struct GamRoom { std::string label; int cont; std::string label_low; };
    struct GamRegion { std::string disp; std::string disp_low;
                       std::vector<GamRoom> rooms; std::vector<GamRoom> objs; };
    std::vector<GamRegion> gam_regions;
    int gam_regions_items = -1;
    poq::ItemNames gam_item_names;         // 物品中文名(别名->中文)
    bool gam_item_names_loaded = false;
    char gam_ginfo_buf[32768] = {0};         // 词条文本编辑缓冲(UTF-8)
    bool gam_ginfo_pending = false;          // 词条已改待保存
    std::vector<poq::GamItem::GInfoRow> gam_grows;  // 当前物品词条(编辑用, 隐藏头行)
    std::vector<uint8_t> gam_grow_sel;               // 与 gam_grows 平行的勾选状态
    std::string gam_gheader;                  // 当前物品词条头行(GBK, 保存时装配回)
    std::string gam_gtext_loaded;             // 装载时装配出的 G_Info(判断用户是否真的改过)
    char gam_alias_buf[64] = {0};             // 物品别名(拼音)编辑缓冲
    char gam_name_buf[128] = {0};             // 物品中文名编辑缓冲(UTF-8)

    // ---- 状态 / 静态选项 ----
    bool hidden = true;  // F12 前隐藏调试 tab
    bool show_welcome = true;  // 启动免责声明弹窗(每次打开应用显示一次)
    std::string status;
    std::vector<uint32_t> attr_ids, color_ids, percent_ids;

    App() : cfg(OffsetsConfig::Default()), modder(cfg) {
        basic_fields = cfg.Current().basic_fields;
        RebuildBasic();
        char_widgets.assign(CharFields().size(), CharWidget());  // 属性修改网格(未读取时全空)
        for (const auto& kv : ItemData::AttrNames()) attr_ids.push_back(kv.first);
        for (const auto& kv : ItemData::ColorNames()) color_ids.push_back(kv.first);
        for (const auto& kv : ItemData::PercentNames()) percent_ids.push_back(kv.first);
        RefreshProcesses();
        ReloadCfgBuffers();
        std::snprintf(gm_count_buf, sizeof(gm_count_buf), "1");  // 执行次数默认 1
        // 无中生有物品目录: exe 目录 / data 子目录 / 开发目录 ../../data(全装备解析产物)
        if (gam_catalog.Load({ WideToUtf8(AppDirW() + L"\\gam_catalog.bin"),
                               WideToUtf8(AppDirW() + L"\\data\\gam_catalog.bin"),
                               WideToUtf8(AppDirW() + L"\\..\\..\\data\\gam_catalog.bin") },
                             gam_catalog)) {
            gam_cat_loaded = true;
        }
        if (gam_scene_cat.Load({ WideToUtf8(AppDirW() + L"\\scene_catalog.bin"),
                                 WideToUtf8(AppDirW() + L"\\data\\scene_catalog.bin"),
                                 WideToUtf8(AppDirW() + L"\\..\\..\\data\\scene_catalog.bin") },
                               gam_scene_cat)) {
            gam_scene_loaded = true;
        }
        if (gam_item_names.Load({ WideToUtf8(AppDirW() + L"\\item_names.bin"),
                                  WideToUtf8(AppDirW() + L"\\data\\item_names.bin"),
                                  WideToUtf8(AppDirW() + L"\\..\\..\\data\\item_names.bin") },
                                gam_item_names)) {
            gam_item_names_loaded = true;
        }
        status = "就绪。请以管理员身份运行,初始化后在游戏中拾起物品。";
    }

    // 重建基础属性控件(配置变化时)
    void RebuildBasic() {
        basic.clear();
        for (const auto& f : basic_fields) {
            BasicWidget w;
            w.is_enum = !f.enum_map.empty();
            basic.push_back(w);
        }
    }

    // 刷新进程列表,只列 POQ.EXE(自动选中第一个,避免全系统进程干扰选择)
    void RefreshProcesses() {
        processes.clear();
        for (const auto& p : ProcessMemory::EnumerateProcesses())
            if (ToLower(p.name) == "poq.exe") processes.push_back(p);
        proc_selected = processes.empty() ? -1 : 0;
        std::string msg = "已列出 " + std::to_string(processes.size()) + " 个 POQ.EXE 进程。";
        if (processes.empty())
            msg += "未找到 POQ.EXE,请先启动游戏后点\"刷新进程\"。";
        status = msg;
    }

    void ReloadCfgBuffers() {
        mode_ids.clear();
        for (const auto& kv : cfg.modes) mode_ids.push_back(kv.first);
        // 定位当前模式
        mode_selected = 0;
        for (size_t i = 0; i < mode_ids.size(); ++i)
            if (mode_ids[i] == cfg.current_mode) {
                mode_selected = static_cast<int>(i);
                break;
            }
        const ModeCfg& m = cfg.Current();
        const HookCfg& hk = m.hook;
        const AttrListCfg& al = m.attr_list;
        std::snprintf(cfg_module, sizeof(cfg_module), "%s", hk.module.c_str());
        std::snprintf(cfg_offset, sizeof(cfg_offset), "%s", HexStr(hk.offset).c_str());
        std::snprintf(cfg_aob, sizeof(cfg_aob), "%s", hk.aob.c_str());
        std::snprintf(cfg_weight_offset, sizeof(cfg_weight_offset), "%s", HexStr(hk.weight_offset).c_str());
        std::snprintf(cfg_weight_max, sizeof(cfg_weight_max), "%u", hk.weight_max);
        std::snprintf(cfg_item_adjust, sizeof(cfg_item_adjust), "%d", m.item_base_adjust);
        std::snprintf(cfg_head, sizeof(cfg_head), "%s", HexStr(al.head_offset).c_str());
        std::snprintf(cfg_next, sizeof(cfg_next), "%s", HexStr(al.next_offset).c_str());
        std::snprintf(cfg_id, sizeof(cfg_id), "%s", HexStr(al.id_offset).c_str());
        std::snprintf(cfg_color, sizeof(cfg_color), "%s", HexStr(al.color_offset).c_str());
        std::snprintf(cfg_precent, sizeof(cfg_precent), "%s", HexStr(al.precent_offset).c_str());
        std::snprintf(cfg_value, sizeof(cfg_value), "%s", HexStr(al.value_offset).c_str());
    }

    // 当前"类型"字段值(enum 取 enum_val,否则 intval)
    std::optional<int> CurrentTypeValue() const {
        for (size_t i = 0; i < basic_fields.size(); ++i) {
            if (basic_fields[i].label == "类型") {
                return basic[i].is_enum ? basic[i].enum_val : basic[i].intval;
            }
        }
        return std::nullopt;
    }

    void SetStatus(const std::string& msg) { status = Sanitize(msg, !hidden); }
};

// ============================================================================
// 进程 / 业务逻辑(对应 cpp MainWindow 的 slot)
// ============================================================================

// 前向声明:OnInit 调用 OnCharRead(定义在后面的属性修改逻辑区)。
void OnCharRead(App& app);

std::optional<unsigned int> TryGetSelectedPid(const App& app) {
    if (app.proc_selected >= 0 && static_cast<size_t>(app.proc_selected) < app.processes.size())
        return app.processes[static_cast<size_t>(app.proc_selected)].pid;
    return std::nullopt;
}

std::optional<unsigned int> FindPoqPid(const App& app) {
    for (const auto& p : app.processes)
        if (ToLower(p.name) == "poq.exe") return p.pid;
    return std::nullopt;
}

// 按游戏实际加载的引擎模块自动选版本模式:EasyRPG.dll=Steam(mode1),EasyRPG116.dll=v1.30(mode0)。
// 两版本 hook/flag 偏移不同,自动选避免用户手选错。
static void AutoSelectMode(App& app, unsigned int pid) {
    std::string want;
    if (ProcessMemory::FindModuleBase(pid, "EasyRPG.dll")) want = "mode1";
    else if (ProcessMemory::FindModuleBase(pid, "EasyRPG116.dll")) want = "mode0";
    if (want.empty() || want == app.cfg.current_mode) return;
    app.cfg.SetCurrent(want);
    app.modder.Config() = app.cfg;
    app.basic_fields = app.cfg.Current().basic_fields;
    app.RebuildBasic();
    app.ReloadCfgBuffers();
    for (size_t i = 0; i < app.mode_ids.size(); ++i)
        if (app.mode_ids[i] == want) {
            app.mode_selected = static_cast<int>(i);
            break;
        }
}

void OnInit(App& app) {
    auto pid = TryGetSelectedPid(app);
    if (!pid) {
        app.SetStatus("请先选择一个进程。");
        return;
    }
    AutoSelectMode(app, *pid);  // 按引擎模块自动选 steam / v1.30
    bool restarted = false;
    uintptr_t ptr = 0;
    try {
        try {
            ptr = app.modder.Initialize(*pid);
        } catch (const std::exception& ex) {
            // 游戏可能已重启,选中 PID 失效:刷新进程列表后按名重试一次
            if (std::string(ex.what()).find("OpenProcess") == std::string::npos) throw;
            app.RefreshProcesses();
            auto new_pid = FindPoqPid(app);
            if (!new_pid || *new_pid == *pid) throw;
            pid = new_pid;
            for (size_t i = 0; i < app.processes.size(); ++i)
                if (app.processes[i].pid == *new_pid) {
                    app.proc_selected = static_cast<int>(i);
                    break;
                }
            AutoSelectMode(app, *new_pid);  // 重启后按新进程引擎模块重选版本
            ptr = app.modder.Initialize(*pid);
            restarted = true;
        }        uintptr_t slot = app.modder.CurItemSlotAddr();
        std::snprintf(app.raw_addr, sizeof(app.raw_addr), "0x%s", Hex8(slot).c_str());
        const bool dbg = !app.hidden;
        std::string prefix = restarted ? "检测到游戏重启,已自动重新选择进程。" : "";
        const bool injected = app.modder.IsInjected();
        if (dbg) {
            std::string base_info = "hook模块=" + app.cfg.Current().hook.module + " hook@0x" +
                                    Hex8(app.modder.HookAddr()) + " cur_item槽=0x" + Hex8(slot);
            if (!injected) {
                app.SetStatus(prefix + "初始化成功(注入失败,道具修改不可用)。请重新初始化。" +
                              base_info);
            } else if (ptr == 0 || ptr == 0xFFFFFFFF) {
                app.SetStatus(prefix +
                              "初始化成功(已注入)。当前无手持物品,请在游戏中拾起物品后点\"读取属性\"。" +
                              base_info);
            } else {
                app.SetStatus(prefix + "初始化成功(已注入)。手持物品指针 = 0x" + Hex8(ptr) +
                              base_info);
            }
        } else {
            if (!injected) {
                app.SetStatus(prefix +
                              "初始化成功(注入失败,道具修改不可用)。请重新初始化。");
            } else if (ptr == 0 || ptr == 0xFFFFFFFF) {
                app.SetStatus(prefix +
                              "初始化成功(已注入)。当前无手持物品,请在游戏中拾起物品后点\"读取属性\"。");
            } else {
                app.SetStatus(prefix + "初始化成功(已注入)。");
            }
        }
        // 初始化后自动加载角色属性(对齐 Python _on_init)
        try {
            OnCharRead(app);
        } catch (...) {
        }
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("初始化失败", msg, MB_ICONERROR);
        app.SetStatus("初始化失败:" + msg);
    }
}

void FillBasic(App& app, const BasicState& st) {
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        App::BasicWidget& w = app.basic[i];
        if (f.type == "string_ptr") {
            auto it = st.strs.find(f.label);
            if (it != st.strs.end())
                std::snprintf(w.strbuf, sizeof(w.strbuf), "%s", it->second.c_str());
        } else {
            auto it = st.ints.find(f.label);
            if (it != st.ints.end()) {
                if (w.is_enum) w.enum_val = it->second;
                else w.intval = it->second;
            }
        }
    }
}

// 把界面上的 道具名字/道具别名/类型 记进快照(供"复制上一件物品"按需复制)
void SnapshotNames(App& app) {
    app.last_name.clear();
    app.last_alias.clear();
    app.last_type = -1;
    for (size_t i = 0; i < app.basic_fields.size() && i < app.basic.size(); ++i) {
        const std::string& lb = app.basic_fields[i].label;
        if (lb == "道具名字") app.last_name = app.basic[i].strbuf;
        else if (lb == "道具别名") app.last_alias = app.basic[i].strbuf;
        else if (lb == "类型")
            app.last_type = app.basic[i].is_enum ? app.basic[i].enum_val : app.basic[i].intval;
    }
}

// 字段值的可读显示: 枚举字段(如 类型/五行)显示成"值：名字", 与下拉框一致; 其它直接数字。
std::string FieldValueDisplay(const App& app, const char* label, int v) {
    for (const auto& f : app.cfg.Current().basic_fields) {
        if (f.label != label) continue;
        auto it = f.enum_map.find(v);
        if (it != f.enum_map.end())
            return f.enum_display_value ? (std::to_string(v) + "：" + it->second) : it->second;
        break;
    }
    return std::to_string(v);
}

BasicState CollectBasicState(const App& app) {
    BasicState st;
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        const App::BasicWidget& w = app.basic[i];
        if (f.type == "string_ptr") st.strs[f.label] = w.strbuf;
        else if (w.is_enum) st.ints[f.label] = w.enum_val;
        else st.ints[f.label] = w.intval;
    }
    return st;
}

// 检测游戏窗口内左键按下(上升沿),触发"点击捕获":
// 解锁->清槽->等游戏把被点中的物品读进槽->一到手立即冻结(防被后续悬停/场景顶掉)。
// 只在已注入且支持冻结时生效;点击发生在我们窗口上不算。
// 从物品基址读取基础字段+属性表格,更新快照与状态(「读取属性」的公共主体)。
void ReadFromBase(App& app, uintptr_t ib) {
    FillBasic(app, app.modder.ReadBasicFields(ib));
    app.attrs = app.modder.ReadAttributes(ib);
    app.attr_sel.assign(app.attrs.size(), false);
    app.last_attrs = app.attrs;  // 快照:供"复制上一件物品"用
    SnapshotNames(app);          // 名字/别名也快照(复制时询问是否带过来)
    app.last_item_base = ib;
    if (!app.hidden) {
        app.SetStatus("读取完成。物品基址 = 0x" + Hex8(ib) + ",属性 " +
                      std::to_string(app.attrs.size()) + " 条。");
    } else {
        app.SetStatus("读取完成,属性 " + std::to_string(app.attrs.size()) + " 条。");
    }
}

void OnRead(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    try {
        uintptr_t ib = 0;
        if (app.modder.BagFlagActive()) {
            // 背包拿起边缘门:闩锁 = 最近一次"从背包拿起(0->1)"的物品,读它即可。
            // 悬停/场景读取在 flag 平直期不会入闩锁,无需再等捕获。
            uintptr_t p = app.modder.GetItemPointer();
            if (p < 0x10000) {
                app.SetStatus(
                    "请先在游戏里从背包拿起一件物品(点一下让图标贴到光标),再点\"读取属性\"。");
                return;
            }
            ib = p + app.modder.Config().Current().item_base_adjust;
        } else if (app.modder.CaptureFreezeSupported()) {
            // 解冻让新捕获可通过;绝不清槽——不匹配门控的读取不改 cur_item,
            // 失败也保持原地址(只会被 cave 的合法捕获改写)。
            app.SetStatus("正在捕获手持物品…(请保持武器握在手上)");
            app.modder.SetCaptureFrozen(false);
            for (int i = 0; i < 60; ++i) {  // 最长 ~3 秒
                uintptr_t p = app.modder.GetItemPointer();
                if (p >= 0x10000) {
                    ib = p + app.modder.Config().Current().item_base_adjust;
                    break;
                }
                Sleep(50);
            }
            app.modder.SetCaptureFrozen(true);
            if (!ib) {
                app.SetStatus("未读到手持物品,请手持武器后重试(cur_item 保持原地址不变)。");
                return;
            }
        } else {
            ib = app.modder.GetItemBase();
        }
        ReadFromBase(app, ib);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("读取失败", msg, MB_ICONERROR);
        app.SetStatus("读取失败:" + msg);
    }
}

void OnSave(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    auto props = app.attrs;
    try {
        uintptr_t ib = app.modder.GetItemBase();
        BasicState bs = CollectBasicState(app);
        app.modder.WriteBasicFields(ib, bs);
        auto written_skipped = app.modder.WriteAttributes(ib, props);
        int written = written_skipped.first;
        int skipped = written_skipped.second;
        // 保存成功后刷新快照(含用户编辑后的值),供"复制上一件物品"用。
        app.last_attrs = props;
        SnapshotNames(app);
        app.last_item_base = ib;
        std::string msg = "保存完成:基础属性 " + std::to_string(bs.ints.size() + bs.strs.size()) +
                          " 项,属性 " + std::to_string(written) + " 条已写入。";
        if (skipped > 0) {
            msg += "(" + std::to_string(skipped) + " 条新增属性因节点池无空闲槽被跳过)";
            MsgBox("部分属性未写入",
                   "有 " + std::to_string(skipped) +
                       " 条新增属性因节点池无空闲槽被跳过。\n"
                       "若刚丢弃过武器或读档过,可点击\"重置节点池\"回收全部槽位后重试。\n"
                       "已写入的属性不受影响。",
                   MB_ICONWARNING);
        }
        msg += "请在游戏中把物品扔到地上再捡起使修改生效。";
        app.SetStatus(msg);
        MsgBox("修改成功",
               "请在游戏中把物品扔到地上在捡起来使修改生效",
               MB_ICONINFORMATION);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("保存失败", msg, MB_ICONERROR);
        app.SetStatus("保存失败:" + msg);
    }
}

static void ApplyCopyPrevItem(App& app, bool withIdent);   // 先问后做: 真正写内存

// 把缓存的上一件物品属性写到当前物品:
// 现有节点原位覆盖(前 min(m,k) 条),多出来的词条从节点池分配接到链尾,
// 少了则截断。名字/别名/类型按用户选择决定是否一起写。
void OnCopyPrevItem(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    if (app.last_attrs.empty()) {
        app.SetStatus("还没有读取过任何物品的属性,无法复制。");
        return;
    }
    // 先问名字/别名/类型要不要一起复制, 再动手(取消 = 什么都不做)
    std::string q = "上一件物品:";
    if (!app.last_name.empty()) q += "\n  名字: " + app.last_name;
    if (!app.last_alias.empty()) q += "\n  别名: " + app.last_alias;
    if (app.last_type >= 0) q += "\n  类型: " + FieldValueDisplay(app, "类型", app.last_type);
    q += "\n\n这三项(名字/别名/类型)要不要一起复制过来？词条一定复制。";
    App* ap = &app;
    MsgBoxAsk("复制上一件物品", q, [ap](int choice) {
        if (choice == 2) {
            ap->SetStatus("已取消复制上一件物品(什么都没改)。");
            return;
        }
        ApplyCopyPrevItem(*ap, choice == 0);
    });
}

// 真正做"复制上一件物品": withIdent=true 连名字/别名/类型一起写。
static void ApplyCopyPrevItem(App& app, bool withIdent) {
    try {
        uintptr_t ib = app.modder.GetItemBase();
        auto written_skipped = app.modder.WriteAttributes(ib, app.last_attrs);
        int written = written_skipped.first;
        int skipped = written_skipped.second;
        std::string msg = std::string(withIdent ? "已复制上一件物品的属性+名字/别名/类型,写入 "
                                                : "已复制上一件物品的属性(名字/别名/类型保持原样),写入 ") +
                          std::to_string(written) + " 条。";
        if (ib == app.last_item_base) {
            msg += "(注意:当前物品就是缓存的那件)";
        }
        if (skipped > 0) {
            msg += "(" + std::to_string(skipped) +
                   " 条因节点池无空闲槽被跳过,可点\"重置节点池\"后重试)";
        }
        msg += "请在游戏中把物品扔到地上再捡起使修改生效。";
        // 表格同步为已写入的内容,与内存一致
        app.attrs = app.last_attrs;
        app.attr_sel.assign(app.attrs.size(), false);
        if (withIdent) {
            BasicState bs;
            if (!app.last_name.empty()) bs.strs["道具名字"] = app.last_name;
            if (!app.last_alias.empty()) bs.strs["道具别名"] = app.last_alias;
            if (app.last_type >= 0) bs.ints["类型"] = app.last_type;
            if (!bs.strs.empty() || !bs.ints.empty()) app.modder.WriteBasicFields(ib, bs);
            for (size_t i = 0; i < app.basic_fields.size() && i < app.basic.size(); ++i) {
                const std::string& lb = app.basic_fields[i].label;
                if (!app.last_name.empty() && lb == "道具名字")
                    std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                                  app.last_name.c_str());
                else if (!app.last_alias.empty() && lb == "道具别名")
                    std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                                  app.last_alias.c_str());
                else if (app.last_type >= 0 && lb == "类型") {
                    if (app.basic[i].is_enum) app.basic[i].enum_val = app.last_type;
                    else app.basic[i].intval = app.last_type;
                }
            }
        }
        app.SetStatus(msg);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        app.SetStatus("复制失败:" + msg);
    }
}

static void ApplyLoadedTpl(App& app, const TemplateLoadResult& res, bool withIdent);

void OnLoadTpl(App& app) {
    std::string path = OpenFileDialog(AppDirW(), L"JSON 文件 (*.json)\0*.json\0所有文件 (*)\0*.*\0\0");
    if (path.empty()) return;
    TemplateLoadResult res;
    try {
        res = JsonTemplate::Load(path, app.basic_fields);
    } catch (const std::exception& ex) {
        MsgBox("加载失败", "加载 JSON 失败,什么都不做:\n" + Sanitize(ex.what(), !app.hidden),
               MB_ICONERROR);
        return;
    }
    // 基础属性:填入可写字段(只读字段不覆盖)
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        auto it = res.basic_values.find(f.label);
        if (it == res.basic_values.end()) continue;
        App::BasicWidget& w = app.basic[i];
        if (f.type == "string_ptr") {
            std::snprintf(w.strbuf, sizeof(w.strbuf), "%d", it->second);
        } else if (w.is_enum) {
            w.enum_val = it->second;
        } else {
            w.intval = it->second;
        }
    }
    app.attrs = res.attrs;
    app.attr_sel.assign(app.attrs.size(), false);
    if (!res.errors.empty()) {
        std::string msg = "部分属性存在错误,跳过加载相关词条,请检查原始json\n\n";
        for (const auto& e : res.errors) msg += e + "\n";
        MsgBox("部分属性存在错误", msg, MB_ICONWARNING);
    }
    // 名字/别名/类型: 模板里有就先问要不要一起导入, 再动手(取消 = 什么都不做)
    std::string nm, al, ty;
    {
        auto n1 = res.basic_strings.find("道具名字");
        if (n1 != res.basic_strings.end()) nm = n1->second;
        auto a1 = res.basic_strings.find("道具别名");
        if (a1 != res.basic_strings.end()) al = a1->second;
        auto t1 = res.basic_values.find("类型");
        if (t1 != res.basic_values.end()) ty = FieldValueDisplay(app, "类型", t1->second);
    }
    if (nm.empty() && al.empty() && ty.empty()) {
        ApplyLoadedTpl(app, res, true);   // 没有这三项, 直接全量导入
        return;
    }
    std::string q = "模板里带有:";
    if (!nm.empty()) q += "\n  道具名字: " + nm;
    if (!al.empty()) q += "\n  道具别名: " + al;
    if (!ty.empty()) q += "\n  类型: " + ty;
    q += "\n\n这三项(名字/别名/类型)要不要一起导入？词条一定导入。";
    App* ap = &app;
    MsgBoxAsk("导入模板", q, [ap, res](int choice) {
        if (choice == 2) {
            ap->SetStatus("已取消导入(什么都没改)。");
            return;
        }
        ApplyLoadedTpl(*ap, res, choice == 0);
    });
}

// 真正应用模板: withIdent=false 时不导入 类型 与 名字/别名, 其余照常。
static void ApplyLoadedTpl(App& app, const TemplateLoadResult& res, bool withIdent) {
    for (size_t i = 0; i < app.basic_fields.size(); ++i) {
        const auto& f = app.basic_fields[i];
        auto it = res.basic_values.find(f.label);
        if (it == res.basic_values.end()) continue;
        if (!withIdent && f.label == "类型") continue;   // 类型归入"名字/别名"一档
        App::BasicWidget& w = app.basic[i];
        if (f.type == "string_ptr") {
            std::snprintf(w.strbuf, sizeof(w.strbuf), "%d", it->second);
        } else if (w.is_enum) {
            w.enum_val = it->second;
        } else {
            w.intval = it->second;
        }
    }
    app.attrs = res.attrs;
    app.attr_sel.assign(app.attrs.size(), false);
    if (withIdent) {
        auto n1 = res.basic_strings.find("道具名字");
        auto a1 = res.basic_strings.find("道具别名");
        for (size_t i = 0; i < app.basic_fields.size() && i < app.basic.size(); ++i) {
            const std::string& lb = app.basic_fields[i].label;
            if (n1 != res.basic_strings.end() && lb == "道具名字")
                std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                              n1->second.c_str());
            else if (a1 != res.basic_strings.end() && lb == "道具别名")
                std::snprintf(app.basic[i].strbuf, sizeof(app.basic[i].strbuf), "%s",
                              a1->second.c_str());
        }
    }
    if (!res.errors.empty()) {
        std::string msg = "部分属性存在错误,跳过加载相关词条,请检查原始json\n\n";
        for (const auto& e : res.errors) msg += e + "\n";
        MsgBox("部分属性存在错误", msg, MB_ICONWARNING);
    }
    app.SetStatus(std::string("已从模板载入:") +
                  (withIdent ? "含名字/别名/类型," : "名字/别名/类型未导入,") + "基础属性 " +
                  std::to_string(res.basic_values.size()) + " 项,属性 " +
                  std::to_string(res.attrs.size()) + " 条。");
}

void OnSaveAs(App& app) {
    std::string path = SaveFileDialog(AppDirW(), L"JSON 文件 (*.json)\0*.json\0\0");
    if (path.empty()) return;
    try {
        BasicState st = CollectBasicState(app);
        JsonTemplate::Save(path, st, app.basic_fields, app.attrs);
        app.SetStatus("已导出到 " + path + "。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("导出失败", msg, MB_ICONERROR);
        app.SetStatus("导出失败:" + msg);
    }
}

void OnRawRead(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先\"初始化\"。");
        return;
    }
    try {
        uintptr_t addr = ProcessMemory::ParseAddress(Trim(app.raw_addr));
        int size = 64;
        char* end = nullptr;
        long parsed = std::strtol(app.raw_size, &end, 10);
        if (end != app.raw_size && *end == '\0' && parsed > 0) size = static_cast<int>(parsed);
        if (size > 4000) size = 4000;  // 防 buffer 溢出
        auto data = app.modder.ReadMemoryDump(addr, size);
        std::string dump = FormatHexDump(addr, data);
        std::snprintf(app.raw_hex, sizeof(app.raw_hex), "%s", dump.c_str());
        app.SetStatus("已读取 0x" + Hex8(addr) + " 共 " + std::to_string(data.size()) + " 字节。");
    } catch (const std::exception& ex) {
        app.SetStatus("读取失败:" + Sanitize(ex.what(), !app.hidden));
    }
}

void OnRawWrite(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先\"初始化\"。");
        return;
    }
    try {
        uintptr_t addr = ProcessMemory::ParseAddress(Trim(app.raw_addr));
        auto data = ParseHexDump(app.raw_hex);
        if (data.empty()) {
            app.SetStatus("没有可写入的十六进制字节。");
            return;
        }
        app.modder.Mem().Write(addr, data);
        app.SetStatus("已写入 0x" + Hex8(addr) + " 共 " + std::to_string(data.size()) + " 字节。");
    } catch (const std::exception& ex) {
        app.SetStatus("写入失败:" + Sanitize(ex.what(), !app.hidden));
    }
}

void OnSaveCfg(App& app) {
    try {
        if (app.mode_selected < 0 || static_cast<size_t>(app.mode_selected) >= app.mode_ids.size())
            throw std::runtime_error("无效模式");
        const std::string& mid = app.mode_ids[static_cast<size_t>(app.mode_selected)];
        auto mit = app.cfg.modes.find(mid);
        if (mit == app.cfg.modes.end()) throw std::runtime_error("无效模式");
        ModeCfg& m = mit->second;
        auto parse_hex = [](const char* s) -> uint32_t {
            return static_cast<uint32_t>(OffsetsConfig::ParseLong(Trim(s)));
        };
        auto parse_dec = [](const char* key, const char* s) -> long long {
            std::string t = Trim(s);
            if (t.empty()) throw std::runtime_error(std::string(key) + " 不是整数");
            char* end = nullptr;
            long long v = std::strtoll(t.c_str(), &end, 10);
            if (end == t.c_str() || *end != '\0') throw std::runtime_error(std::string(key) + " 不是整数");
            return v;
        };

        m.hook.module = app.cfg_module;
        m.hook.offset = parse_hex(app.cfg_offset);
        m.hook.aob = app.cfg_aob;
        m.hook.weight_offset = parse_hex(app.cfg_weight_offset);
        m.hook.weight_max = static_cast<uint32_t>(parse_dec("hook.weight_max", app.cfg_weight_max));
        m.item_base_adjust = static_cast<int>(parse_dec("item_base_adjust", app.cfg_item_adjust));
        AttrListCfg& al = m.attr_list;
        al.head_offset = parse_hex(app.cfg_head);
        al.next_offset = parse_hex(app.cfg_next);
        al.id_offset = parse_hex(app.cfg_id);
        al.color_offset = parse_hex(app.cfg_color);
        al.precent_offset = parse_hex(app.cfg_precent);
        al.value_offset = parse_hex(app.cfg_value);
        app.cfg.SetCurrent(mid);
        // 单文件版:偏移只保存在内存中,不写配置文件(重启回到内置默认)。
        app.modder.Config() = app.cfg;
        app.basic_fields = app.cfg.Current().basic_fields;
        app.RebuildBasic();
        app.ReloadCfgBuffers();
        app.SetStatus("配置已应用(仅本次会话,重启后回到内置默认)。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("保存配置失败", msg, MB_ICONERROR);
        app.SetStatus("保存配置失败:" + msg);
    }
}

// ============================================================================
// 属性修改(角色)逻辑(对应 Python main_window.py 的 _on_char_* / _lock_*)
// ============================================================================

// 无敌模式涉及的防御字段(普通防御 + 五行防御)。
const std::set<std::string>& DefLabels() {
    static const std::set<std::string> s = {"普通防御", "金防御", "木防御", "水防御", "火防御", "土防御"};
    return s;
}
// 一击必杀涉及的攻击字段(普通攻击 + 五行攻击,均为 pair)。
const std::set<std::string>& AtkLabels() {
    static const std::set<std::string> s = {"普通攻击", "金攻击", "木攻击", "水攻击", "火攻击", "土攻击"};
    return s;
}

// 字段在当前锁状态下是否应禁用编辑(无敌:生命/精气/防御/闪避率;一击必杀:攻击/命中率)。
bool IsCharFieldLocked(const App& app, const std::string& label) {
    if (app.ck_invincible) {
        if (label == "生命" || label == "精气" || label == "闪避率" || DefLabels().count(label)) return true;
    }
    if (app.ck_kill) {
        if (label == "命中率" || AtkLabels().count(label)) return true;
    }
    return false;
}

// 按 label 查 CharFields 索引(找不到返回 -1)。
int FindCharField(const std::string& label) {
    const auto& fs = CharFields();
    for (size_t i = 0; i < fs.size(); ++i)
        if (fs[i].label == label) return static_cast<int>(i);
    return -1;
}

// 为当前选中角色重建字段控件(从内存读取并填充)。
void RebuildCharWidgets(App& app) {
    if (app.char_names.empty() || app.char_selected < 0 ||
        static_cast<size_t>(app.char_selected) >= app.char_names.size()) {
        app.char_widgets.clear();
        return;
    }
    const std::string& name = app.char_names[static_cast<size_t>(app.char_selected)];
    auto it = app.char_addrs.find(name);
    if (it == app.char_addrs.end()) {
        app.char_widgets.clear();
        return;
    }
    uintptr_t base = it->second;
    app.char_widgets.assign(CharFields().size(), CharWidget());
    auto values = ReadAllChar(app.modder.Mem(), base);
    for (size_t i = 0; i < CharFields().size(); ++i) {
        const auto& f = CharFields()[i];
        CharWidget& w = app.char_widgets[i];
        auto vit = values.find(f.label);
        if (vit == values.end()) continue;  // 读取失败:保留默认 0
        const CharValue& v = vit->second;
        if (f.kind == CharKind::StringPtr) {
            std::snprintf(w.strbuf, sizeof(w.strbuf), "%s", v.str.c_str());
        } else if (f.kind == CharKind::Pair) {
            w.pair_lo = v.lo;
            w.pair_hi = v.hi;
        } else {
            w.intval = v.lo;
        }
    }
}

// 读取全部队员:用固定偏移定位(按当前模式的引擎模块选偏移表),填充下拉框 + 全局属性。
void OnCharRead(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.char_status = "请先点击\"初始化\"。";
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    try {
        const std::string& module = app.cfg.Current().hook.module;  // steam/v1.30
        auto located = LocateByOffsets(app.modder.Mem(), app.modder.Pid(), module,
                                       app.char_filter);
        if (app.hidden)
            app.char_slot_probe.clear();
        else
            app.char_slot_probe = ProbeSlotNames(app.modder.Mem(), app.modder.Pid(), module);
        if (located.empty()) {
            app.char_status = "未定位到队员(可尝试取消过滤后再次读取属性)。";
            app.SetStatus("未定位到队员(可尝试取消过滤后再次读取属性)。");
            return;
        }
        app.char_addrs.clear();
        app.char_names.clear();
        for (auto& kv : located) {
            app.char_addrs[kv.first] = kv.second;  // map 供按名查找
            app.char_names.push_back(kv.first);     // 槽位顺序保留下拉框
        }
        app.char_selected = 0;
        // 全局属性:从第 1 人读
        const std::string& first = app.char_names.front();
        auto g = ReadGlobalChar(app.modder.Mem(), app.char_addrs[first]);
        auto mit = g.find("金钱");
        if (mit != g.end()) std::snprintf(app.money_buf, sizeof(app.money_buf), "%d", mit->second.lo);
        RebuildCharWidgets(app);
        app.char_status = "已定位 " + std::to_string(located.size()) + " 人。";
        app.SetStatus("读取完成:定位 " + std::to_string(located.size()) + " 人。");
    } catch (const std::exception& ex) {
        app.char_status = "读取失败:" + Sanitize(ex.what(), !app.hidden);
        app.SetStatus("读取失败:" + Sanitize(ex.what(), !app.hidden));
    }
}

// 保存当前选中角色属性:收集 -> 校验(有非法则整体拒绝) -> 写入。
void OnCharSave(App& app) {
    if (!app.modder.Mem().IsOpen()) {
        app.SetStatus("请先点击\"初始化\"。");
        return;
    }
    if (app.char_addrs.empty()) {
        app.SetStatus("请先\"读取属性\"定位队员。");
        return;
    }
    if (app.char_selected < 0 || static_cast<size_t>(app.char_selected) >= app.char_names.size()) {
        app.SetStatus("请先选择角色。");
        return;
    }
    const std::string& name = app.char_names[static_cast<size_t>(app.char_selected)];
    uintptr_t base = app.char_addrs[name];
    // 1) 收集全部字段值
    std::map<std::string, CharValue> values;
    for (size_t i = 0; i < CharFields().size(); ++i) {
        const auto& f = CharFields()[i];
        const CharWidget& w = app.char_widgets[i];
        CharValue v;
        if (f.kind == CharKind::StringPtr) {
            v.str = w.strbuf;
        } else if (f.kind == CharKind::Pair) {
            v.is_pair = true;
            v.lo = w.pair_lo;
            v.hi = w.pair_hi;
        } else {
            v.lo = w.intval;
        }
        values[f.label] = v;
    }
    // 2) 校验全部字段(有非法则弹窗并拒绝,整次不写)
    std::vector<std::string> errors;
    for (const auto& f : CharFields()) {
        auto err = ValidateValue(f, values[f.label]);
        if (err) errors.push_back(*err);
    }
    if (!errors.empty()) {
        std::string msg = "以下字段不合法,未保存:\n\n";
        for (const auto& e : errors) msg += e + "\n";
        MsgBox("校验失败", msg, MB_ICONERROR);
        app.SetStatus("校验失败,未保存:" + name + "。");
        return;
    }
    // 3) 校验通过,写入
    int saved = 0;
    for (const auto& f : CharFields()) {
        if (WriteCharField(app.modder.Mem(), base, f, values[f.label])) ++saved;
    }
    app.char_status = "保存完成:" + name + " " + std::to_string(saved) + " 个字段已写入。请在游戏中验证。";
    app.SetStatus("角色属性保存完成:" + name + "。");
    MsgBox("保存完成",
           "请存档后重新读档查看变化，部分属性词条可能因道具属性加成与设定值不同",
           MB_ICONINFORMATION);
}

// 修改金钱按钮:校验后仅写第 1 人 +0x2C。
void OnMoneyApply(App& app) {
    if (!app.modder.Mem().IsOpen() || app.char_addrs.empty()) {
        app.SetStatus("请先初始化并读取属性。");
        return;
    }
    const auto& gf = GlobalFields().front();  // 金钱
    std::string t = Trim(app.money_buf);
    char* end = nullptr;
    long long val = std::strtoll(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0') {
        MsgBox("输入错误", "金钱必须是整数(0 ~ 99999999)。", MB_ICONWARNING);
        return;
    }
    CharValue v;
    v.lo = static_cast<int>(val);
    auto err = ValidateValue(gf, v);
    if (err) {
        MsgBox("输入错误", *err, MB_ICONWARNING);
        return;
    }
    const std::string& first = app.char_names.front();
    WriteCharField(app.modder.Mem(), app.char_addrs[first], gf, v);
    MsgBox("金钱已修改", "修改后丢弃金钱、购买商品、捡起金钱，数额才会更新", MB_ICONINFORMATION);
}

// ---- 无敌 / 一击必杀锁 ----
// 立即把已启用锁的字段写为 9999(对所有队员,不等定时器)。
void LockWriteOnce(App& app) {
    if (!app.modder.Mem().IsOpen() || app.char_addrs.empty()) return;
    if (!app.ck_invincible && !app.ck_kill) return;
    for (const auto& kv : app.char_addrs) {
        uintptr_t base = kv.second;
        try {
            if (app.ck_invincible) {
                app.modder.Mem().WriteUInt32(base + 0x30, 9999);  // 当前生命
                app.modder.Mem().WriteUInt32(base + 0x38, 9999);  // 当前精气
                for (const auto& f : CharFields()) {
                    if (DefLabels().count(f.label) || f.label == "闪避率") {
                        CharValue v;
                        v.lo = 9999;
                        WriteCharField(app.modder.Mem(), base, f, v);
                    }
                }
            }
            if (app.ck_kill) {
                for (const auto& f : CharFields()) {
                    if (AtkLabels().count(f.label)) {
                        CharValue v;
                        v.is_pair = true;
                        v.lo = 9999;
                        v.hi = 9999;
                        WriteCharField(app.modder.Mem(), base, f, v);
                    } else if (f.label == "命中率") {
                        CharValue v;
                        v.lo = 9999;
                        WriteCharField(app.modder.Mem(), base, f, v);
                    }
                }
            }
        } catch (...) {
        }
    }
}

// 无敌模式勾选/取消。
void OnInvincibleToggled(App& app, bool checked) {
    if (checked) {
        // 缓存防御/闪避原值(所有队员)
        app.def_cache.clear();
        for (const auto& kv : app.char_addrs) {
            for (const auto& f : CharFields()) {
                if (DefLabels().count(f.label) || f.label == "闪避率") {
                    auto v = ReadCharField(app.modder.Mem(), kv.second, f);
                    if (v) app.def_cache[kv.first][f.label] = *v;
                }
            }
        }
        LockWriteOnce(app);  // 立即写 9999
    } else {
        // 取消:生命=最大生命、精气=最大精气、防御+闪避=缓存值
        for (const auto& kv : app.char_addrs) {
            uintptr_t base = kv.second;
            try {
                unsigned int mx_hp = app.modder.Mem().ReadUInt32(base + 0x34);
                app.modder.Mem().WriteUInt32(base + 0x30, mx_hp);
                unsigned int mx_mp = app.modder.Mem().ReadUInt32(base + 0x3C);
                app.modder.Mem().WriteUInt32(base + 0x38, mx_mp);
                for (const auto& f : CharFields()) {
                    if (DefLabels().count(f.label) || f.label == "闪避率") {
                        auto cit = app.def_cache.find(kv.first);
                        if (cit != app.def_cache.end()) {
                            auto fit = cit->second.find(f.label);
                            if (fit != cit->second.end())
                                WriteCharField(app.modder.Mem(), base, f, fit->second);
                        }
                    }
                }
            } catch (...) {
            }
        }
        app.def_cache.clear();
    }
}

// 一击必杀勾选/取消。
void OnKillToggled(App& app, bool checked) {
    if (checked) {
        // 缓存攻击/命中率原值(所有队员)
        app.hit_cache.clear();
        for (const auto& kv : app.char_addrs) {
            for (const auto& f : CharFields()) {
                if (AtkLabels().count(f.label) || f.label == "命中率") {
                    auto v = ReadCharField(app.modder.Mem(), kv.second, f);
                    if (v) app.hit_cache[kv.first][f.label] = *v;
                }
            }
        }
        LockWriteOnce(app);  // 立即写 9999
    } else {
        // 取消:攻击/命中率恢复原值
        for (const auto& kv : app.char_addrs) {
            uintptr_t base = kv.second;
            try {
                for (const auto& f : CharFields()) {
                    if (AtkLabels().count(f.label) || f.label == "命中率") {
                        auto cit = app.hit_cache.find(kv.first);
                        if (cit != app.hit_cache.end()) {
                            auto fit = cit->second.find(f.label);
                            if (fit != cit->second.end())
                                WriteCharField(app.modder.Mem(), base, f, fit->second);
                        }
                    }
                }
            } catch (...) {
            }
        }
        app.hit_cache.clear();
    }
}

// 锁定时器:每 500ms 把已启用锁的字段写回 9999(对所有队员)。
void TickLock(App& app, float dt) {
    if (!app.ck_invincible && !app.ck_kill) {
        app.lock_accum = 0.0f;
        return;
    }
    app.lock_accum += dt;
    if (app.lock_accum < 0.5f) return;
    app.lock_accum = 0.0f;
    LockWriteOnce(app);
}

// ============================================================================
// UI 渲染
// ============================================================================

int IndexOf(const std::vector<uint32_t>& ids, uint32_t v) {
    for (size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == v) return static_cast<int>(i);
    return -1;
}

void RenderBasicFields(App& app) {
    auto type_val = app.CurrentTypeValue();
    const float avail = ImGui::GetContentRegionAvail().x;
    // 网格列数:正常宽度下武器名字/别名 2 列一行、其余 3 列一行;窗口窄时递减。
    const int cols = avail >= 900.0f ? 3 : (avail >= 600.0f ? 2 : 1);
    // 前 2 个字段(武器名字/别名)是否单独用 2 列各占半行(窗口太窄时并入后面表格)。
    const size_t head_n = (cols >= 2 && app.basic_fields.size() >= 2) ? 2 : 0;

    // 渲染单个字段(labels 在上、输入框在下、hint 灰字在输入框下方,输入框填满列宽)。
    auto render_field = [&](size_t i) {
        const auto& f = app.basic_fields[i];
        App::BasicWidget& w = app.basic[i];

        // 条件/强制字段:渲染时即时计算(逻辑对齐 cpp UpdateConditionalFields)
        bool forced = f.force_when && f.IsForced(type_val);
        bool cond_inactive = f.conditional && f.conditional->field == "类型" &&
                             !(type_val && std::find(f.conditional->values.begin(),
                                                     f.conditional->values.end(),
                                                     *type_val) !=
                                                 f.conditional->values.end());
        bool disabled = f.readonly || forced || cond_inactive;

        std::string label = f.label;
        if (f.readonly) label += "（只读）";

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::BeginDisabled(disabled);

        if (f.type == "string_ptr") {
            ImGui::InputText(("##b" + std::to_string(i)).c_str(), w.strbuf,
                             sizeof(w.strbuf), ImGuiInputTextFlags_None);
        } else if (w.is_enum) {
            // 下拉选项:值->名字(enum_display_value 显示 "值：名字")
            std::vector<std::pair<int, std::string>> options;
            for (const auto& kv : f.enum_map) {
                std::string item = f.enum_display_value
                                       ? std::to_string(kv.first) + "：" + kv.second
                                       : kv.second;
                options.emplace_back(kv.first, std::move(item));
            }
            std::vector<const char*> items;
            for (const auto& o : options) items.push_back(o.second.c_str());
            int display = w.enum_val;
            if (forced) display = f.force_when->value;
            int cur = -1;
            for (int j = 0; j < static_cast<int>(options.size()); ++j)
                if (options[static_cast<size_t>(j)].first == display) {
                    cur = j;
                    break;
                }
            if (ImGui::Combo(("##b" + std::to_string(i)).c_str(), &cur, items.data(),
                             static_cast<int>(items.size()))) {
                if (cur >= 0) w.enum_val = options[static_cast<size_t>(cur)].first;
            }
        } else {
            int edit = w.intval;
            if (forced) edit = f.force_when->value;
            else if (cond_inactive) edit = -1;
            if (ImGui::InputInt(("##b" + std::to_string(i)).c_str(), &edit)) w.intval = edit;
        }
        ImGui::EndDisabled();
        if (f.hint) {
            ImGui::TextDisabled("%s", f.hint->c_str());
        }
    };

    // 武器名字/武器别名:2 列,各占半行。
    if (head_n > 0) {
        if (ImGui::BeginTable("basichead", 2)) {
            render_field(0);
            render_field(1);
            ImGui::EndTable();
        }
    }
    // 其余属性:3 列一行(窄窗口退化为 2/1 列)。
    if (app.basic_fields.size() > head_n) {
        if (ImGui::BeginTable("basicrest", cols)) {
            for (size_t i = head_n; i < app.basic_fields.size(); ++i) render_field(i);
            ImGui::EndTable();
        }
    }
}

void RenderAttrTable(App& app, float height) {
    // 按钮行
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    if (ImGui::Button("读取属性")) OnRead(app);
    ImGui::SameLine();
    if (ImGui::Button("从模板读取")) OnLoadTpl(app);
    ImGui::SameLine();
    if (ImGui::Button("保存属性")) OnSave(app);
    ImGui::SameLine();
    if (ImGui::Button("另存为")) OnSaveAs(app);
    ImGui::SameLine();
    if (ImGui::Button("添加")) {
        app.attrs.push_back(ItemProperty());
        app.attr_sel.push_back(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("删除")) {
        for (int r = static_cast<int>(app.attrs.size()) - 1; r >= 0; --r) {
            if (r < static_cast<int>(app.attr_sel.size()) && app.attr_sel[static_cast<size_t>(r)]) {
                app.attrs.erase(app.attrs.begin() + r);
                app.attr_sel.erase(app.attr_sel.begin() + r);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("清空")) {
        app.attrs.clear();
        app.attr_sel.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("重置节点池")) {
        if (!app.modder.Mem().IsOpen()) {
            app.SetStatus("请先点击\"初始化\"。");
        } else if (app.modder.ResetPool()) {
            app.SetStatus("节点池已清零,全部槽位恢复可用。");
        } else {
            app.SetStatus("重置节点池失败(池不存在或清零失败)。");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "把新增词条用的节点池整体清零回收。\n"
            "在游戏里丢弃过带新词条的武器、或读档之后,\n"
            "旧链表已作废但池槽不清零会一直占着,点此全部回收。\n"
            "注意:若还有存活武器的词条挂在池内,清零会废掉那些词条。");
    }
    ImGui::SameLine();
    if (ImGui::Button("复制上一件物品")) OnCopyPrevItem(app);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "把上次读取/保存的那件物品的词条,复制到当前物品上。\n"
            "不复制基础属性(名字、耐久等各物品独立)。\n"
            "用法:读取并改好一把武器后,捡起新武器,直接点此按钮。");
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::Text("属性数：%d", static_cast<int>(app.attrs.size()));

    ImGui::Separator();

    static const std::vector<std::string>& name_items = ItemData::AttrDisplayListRef();
    static std::vector<const char*> name_items_c;
    if (name_items_c.empty())
        for (const auto& s : name_items) name_items_c.push_back(s.c_str());
    std::vector<const char*> percent_items_c;
    for (const auto& kv : ItemData::PercentNames()) percent_items_c.push_back(kv.second.c_str());
    // 属性颜色下拉:颜色字段 + node_flag@0x10(隐藏/激活) 合成 6 项。
    // 颜色:0白 1蓝 2浅蓝 3金 4不显示;浅蓝按 active 拆「激活/未激活」两项。
    static const char* kColorStateItems[] = {
        "白色", "蓝色(K)", "浅蓝色激活(C)", "浅蓝色未激活(H)", "金色(Z)", "不显示",
    };
    auto color_to_idx = [](uint32_t c, uint32_t act) -> int {
        switch (c) {
            case 0: return 0;
            case 1: return 1;
            case 2: return act ? 2 : 3;
            case 3: return 4;
            default: return 5;  // 4 及未知 -> 不显示
        }
    };
    auto color_from_idx = [](int idx, uint32_t& c, uint32_t& act) {
        switch (idx) {
            case 0: c = 0; act = 1; break;
            case 1: c = 1; act = 1; break;
            case 2: c = 2; act = 1; break;  // 浅蓝激活
            case 3: c = 2; act = 0; break;  // 浅蓝未激活(隐藏)
            case 4: c = 3; act = 1; break;
            default: c = 4; act = 1; break;
        }
    };

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_NoHostExtendY;
    if (ImGui::BeginTable("attrs", 5, flags, ImVec2(0.0f, height))) {
        ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("属性名", ImGuiTableColumnFlags_WidthFixed, 220.0f);
        ImGui::TableSetupColumn("属性颜色", ImGuiTableColumnFlags_WidthFixed, 178.0f);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("属性值", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableHeadersRow();

        for (int r = 0; r < static_cast<int>(app.attrs.size()); ++r) {
            ItemProperty& p = app.attrs[static_cast<size_t>(r)];
            ImGui::PushID(r);
            ImGui::TableNextRow();
            if (ImGui::TableSetColumnIndex(0)) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                bool sel = app.attr_sel[static_cast<size_t>(r)];
                if (ImGui::Checkbox("##sel", &sel))
                    app.attr_sel[static_cast<size_t>(r)] = sel;
            }
            if (ImGui::TableSetColumnIndex(1)) {
                int cur = IndexOf(app.attr_ids, p.attr_id);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##name", &cur, name_items_c.data(),
                                 static_cast<int>(name_items_c.size()))) {
                    if (cur >= 0) p.attr_id = app.attr_ids[static_cast<size_t>(cur)];
                }
            }
            if (ImGui::TableSetColumnIndex(2)) {
                int cur = color_to_idx(p.color, p.active);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##color", &cur, kColorStateItems, 6)) {
                    color_from_idx(cur, p.color, p.active);
                }
            }
            if (ImGui::TableSetColumnIndex(3)) {
                int cur = IndexOf(app.percent_ids, p.precent);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##prec", &cur, percent_items_c.data(),
                                 static_cast<int>(percent_items_c.size()))) {
                    if (cur >= 0) p.precent = app.percent_ids[static_cast<size_t>(cur)];
                }
            }
            if (ImGui::TableSetColumnIndex(4)) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputFloat("##val", &p.value, 0.0f, 0.0f, "%.6g");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void RenderMemTab(App& app) {
    ImGui::Text("地址(0x...)");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##addr", app.raw_addr, sizeof(app.raw_addr));
    ImGui::SameLine(0, 20);
    ImGui::Text("长度");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::InputText("##size", app.raw_size, sizeof(app.raw_size));
    ImGui::SameLine(0, 20);
    if (ImGui::Button("读取")) OnRawRead(app);
    ImGui::SameLine();
    if (ImGui::Button("写入")) OnRawWrite(app);

    ImGui::Separator();
    float avail_y = ImGui::GetContentRegionAvail().y;
    if (avail_y > 40) avail_y -= 26;  // 给说明留一行
    ImGui::InputTextMultiline("##hex", app.raw_hex, sizeof(app.raw_hex),
                              ImVec2(-FLT_MIN, avail_y), ImGuiInputTextFlags_None);
    ImGui::TextDisabled("查看拾取物品指针附近内存。写入:每行 \"地址: 十六进制字节\" 或纯字节均可。");
}

void RenderCfgTab(App& app) {
    // 模式选择
    std::vector<std::string> mode_labels;
    for (const auto& kv : app.cfg.modes)
        mode_labels.push_back(kv.first + "：" + kv.second.name);
    std::vector<const char*> mode_items;
    for (const auto& s : mode_labels) mode_items.push_back(s.c_str());
    int sel = app.mode_selected;
    ImGui::Text("模式:");
    ImGui::SameLine();
    if (ImGui::Combo("##mode", &sel, mode_items.data(), static_cast<int>(mode_items.size()))) {
        app.mode_selected = sel;
    }

    auto row = [&](const char* label, char* buf, size_t buflen) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(300);
        ImGui::InputText(("##cfg" + std::string(label)).c_str(), buf, buflen);
    };

    ImGui::Separator();
    row("注入模块 (hook.module)", app.cfg_module, sizeof(app.cfg_module));
    row("注入偏移 (hook.offset, 0x...)", app.cfg_offset, sizeof(app.cfg_offset));
    row("原指令字节 (hook.aob)", app.cfg_aob, sizeof(app.cfg_aob));
    row("重量偏移 (hook.weight_offset, 0x...)", app.cfg_weight_offset, sizeof(app.cfg_weight_offset));
    row("重量上限 (hook.weight_max)", app.cfg_weight_max, sizeof(app.cfg_weight_max));
    row("物品基址调整 (item_base_adjust)", app.cfg_item_adjust, sizeof(app.cfg_item_adjust));
    row("属性头偏移 (attr_list.head_offset, 0x...)", app.cfg_head, sizeof(app.cfg_head));
    row("节点 next 偏移 (0x...)", app.cfg_next, sizeof(app.cfg_next));
    row("节点 ID 偏移 (0x...)", app.cfg_id, sizeof(app.cfg_id));
    row("节点 color 偏移 (0x...)", app.cfg_color, sizeof(app.cfg_color));
    row("节点 precent 偏移 (0x...)", app.cfg_precent, sizeof(app.cfg_precent));
    row("节点 value 偏移 (0x...)", app.cfg_value, sizeof(app.cfg_value));

    ImGui::Separator();
    if (ImGui::Button("保存配置并应用")) OnSaveCfg(app);
    ImGui::TextDisabled(
        "说明:以上为当前模式的 magic number,可在 poq 不同版本间切换。修改后点击\"保存配置并应用\""
        "立即生效,但仅本次会话有效(单文件版不写配置文件,重启回到内置默认)。"
        "内置默认在 src/Core/OffsetsConfig.cpp 的 DefaultMode() 中维护,"
        "需要固定偏移时改代码重新编译。");
}

// ============================================================================
// 召唤修改 Tab(task7)
// ============================================================================
// 版本选择复用顶部全局选择器;steam/v1.30 两套召唤逻辑分别调用
// summon_steam / summon_130 独立模块(文件级隔离,不做配置耦合,对齐 python
// summon_tab.py + data/py/summon_tool_{steam,130}.py)。本文件只做 UI 编排。

// UI 数据镜像(对齐 python summon_tab.py: 下拉文案含序号,子类名与工具模块一致)。
const char* kSummonMonNames[10] = {"狼",   "虎",   "熊", "白狼", "木偶",
                                   "狗",   "虎蛟", "狍鸮", "朱厌", "善翼"};
const char* kSummonAssassinNames[2] = {"弓箭刺客", "带刀刺客"};
struct SummonCatUi {
    const char* name;
    std::vector<const char*> subs;
};
const std::vector<SummonCatUi>& SummonQinCats() {
    static const std::vector<SummonCatUi> cats = {
        {"普通兵", {"刀盾", "长戈", "弓箭"}},
        // 队长大类的 '旧重剑' 不支持:数据保留、下拉不显示(对齐 task7 #3.3.1)
        {"队长", {"长斧", "旧重剑", "重剑"}},
        {"大将", {"A", "B", "C", "D", "E"}},
        {"官员", {"官员"}},
        {"法师", {"A", "B", "C", "D", "E"}},
        {"大法师", {"A", "B", "C", "D", "E"}},
    };
    return cats;
}

// 带序号的下拉项("0：狼"...),供 Combo 使用。
std::vector<std::string> MakeMonLabels(const char* const* names, int count) {
    std::vector<std::string> out;
    for (int i = 0; i < count; ++i) out.push_back(std::to_string(i) + "：" + names[i]);
    return out;
}

// 当前 UI 选择的召唤目标字符串(激活状态标签用,对齐 python _current_selection)。
std::string SummonCurrentSelection(const App& app) {
    if (app.cfg.current_mode == "mode1") {  // steam
        if (app.sm_kind == 0) return kSummonMonNames[app.sm_mon];
        if (app.sm_kind == 1) return kSummonAssassinNames[app.sm_assassin];
        if (app.sm_kind == 2) {
            if (app.sm_soldier == 0) return "随机秦兵";
            const SummonCatUi& cat = SummonQinCats()[static_cast<size_t>(app.sm_soldier - 1)];
            std::string sub = (app.sm_sub >= 0) ? cat.subs[static_cast<size_t>(app.sm_sub)] : "";
            return "秦兵-" + std::string(cat.name) + "-" + sub;
        }
        return "狼";  // 全不选:默认召唤狼
    }
    // v1.30
    if (app.sm130_kind == 0) return kSummonMonNames[app.sm130_mon];
    return "随机秦兵";
}

// 按当前 UI 构建工具命名参数(对齐 python _build_args)。
std::vector<std::string> BuildSummonArgs(const App& app) {
    std::vector<std::string> args;
    args.push_back("--group");
    args.push_back(std::to_string(app.sm_group));
    if (app.cfg.current_mode == "mode1") {  // steam
        if (app.sm_kind == 0) {
            args.push_back("--mon_type");
            args.push_back(std::to_string(app.sm_mon));
        } else if (app.sm_kind == 1) {
            args.push_back("--assassin_type");
            args.push_back(std::to_string(app.sm_assassin));
        } else if (app.sm_kind == 2) {
            args.push_back("--soldier_type");
            args.push_back(std::to_string(app.sm_soldier));
            if (app.sm_soldier >= 1 && app.sm_soldier <= 6 && app.sm_sub >= 0) {
                args.push_back("--soldier_sub_type");
                args.push_back(std::to_string(app.sm_sub));
            }
        } else {
            args.push_back("--mon_type");
            args.push_back("0");  // 默认狼
        }
    } else {  // v1.30
        if (app.sm130_kind == 0) {
            args.push_back("--mon_type");
            args.push_back(std::to_string(app.sm130_mon));
        } else if (app.sm130_kind == 1) {
            args.push_back("--soldier_type");
            args.push_back("0");  // 随机秦兵(task7 #4)
        } else {
            args.push_back("--mon_type");
            args.push_back("0");
        }
    }
    return args;
}

void AppendSummonLog(App& app, const std::string& text) {
    if (text.empty()) return;
    if (!app.summon_log.empty()) app.summon_log += "\n";
    app.summon_log += text;
}

// 调用对应版本工具模块(进程内,不调 python)。
// 进程 pid:已初始化(Modder 打开)优先用 Pid();未初始化用顶部下拉选中的进程(对齐 python
// summon_tab._resolve_pid)并临时 OpenProcess(对齐 python 每次自开 mem)。版本检查在下面:
// 所选版本的模块找不到 -> 弹窗提示版本不符。
std::string RunSummonTool(App& app, const std::vector<std::string>& args) {
    unsigned int pid = 0;
    ProcessMemory tmp_mem;  // 未初始化时的临时句柄(作用域结束自动关闭)
    ProcessMemory* mem = nullptr;
    if (app.modder.Mem().IsOpen()) {
        pid = app.modder.Pid();
        mem = &app.modder.Mem();
    } else {
        auto sel = TryGetSelectedPid(app);
        if (!sel) {
            throw std::runtime_error("未选择进程：请先在顶部选择 POQ.EXE 并初始化。");
        }
        pid = *sel;
        if (!tmp_mem.Open(pid)) {
            throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）。");
        }
        mem = &tmp_mem;
    }
    const ModeCfg& m = app.cfg.Current();
    auto base_opt = ProcessMemory::FindModuleBase(pid, m.hook.module);
    if (!base_opt) {
        throw std::runtime_error(
            "未找到模块 " + m.hook.module + "：当前运行的游戏版本与所选版本（" + m.name +
            "）不符，或游戏未运行。请确认游戏正在运行，并在顶部选择正确的游戏版本后重新初始化。");
    }
    if (app.cfg.current_mode == "mode1")
        return summon_steam::CmdSummon(*mem, *base_opt, args);
    return summon_130::CmdSummon(*mem, *base_opt, args);
}

void OnSummonCancel(App& app);  // 前向声明(OnSummonActivate 定义在其之前)

void OnSummonActivate(App& app) {
    if (app.summon_active) {
        OnSummonCancel(app);
        return;
    }
    try {
        std::string text = RunSummonTool(app, BuildSummonArgs(app));
        AppendSummonLog(app, text);
        app.summon_active = true;
        app.SetStatus("召唤修改已激活：力士使用\"烽火狼烟\"即可召唤。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("激活失败", msg, MB_ICONERROR);
        AppendSummonLog(app, "激活失败：" + msg);
        app.SetStatus("激活失败：" + msg);
    }
}

void OnSummonCancel(App& app) {
    try {
        std::string text = RunSummonTool(app, {"--group", "0", "--mon_type", "0"});
        AppendSummonLog(app, text);
        app.summon_active = false;
        app.SetStatus("已取消：恢复到烽火狼烟默认值（召唤狼）。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("取消失败", msg, MB_ICONERROR);
        AppendSummonLog(app, "取消失败：" + msg);
        app.SetStatus("取消失败：" + msg);
    }
}

void OnSummonRefresh(App& app) {
    if (!app.summon_active) return;
    try {
        std::string text = RunSummonTool(app, BuildSummonArgs(app));
        AppendSummonLog(app, text);
        app.SetStatus("已重新设置当前界面选择的怪物类型。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("刷新失败", msg, MB_ICONERROR);
        AppendSummonLog(app, "刷新失败：" + msg);
        app.SetStatus("刷新失败：" + msg);
    }
}

// 两版共有的"召唤归属"下拉(对齐 python _add_group_row)。
void SummonGroupRow(App& app) {
    ImGui::TextUnformatted("召唤归属：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);
    const char* group_items[] = {"1：队友，可控制", "2：盟友，不可控制，被动反击"};
    ImGui::Combo("##sm_group", &app.sm_group, group_items, 2);
}

// Steam 功能区(task7 #3):mon_type / assassin_type / soldier_type(+子类) 三选一。
void RenderSummonSteamFunc(App& app) {
    SummonGroupRow(app);

    // 召唤野兽 (mon_type 0-9)
    bool v = (app.sm_kind == 0);
    if (ImGui::Checkbox("召唤野兽", &v)) app.sm_kind = v ? 0 : -1;
    ImGui::SameLine();
    std::vector<std::string> mon_labels = MakeMonLabels(kSummonMonNames, 10);
    std::vector<const char*> mon_items;
    for (const auto& s : mon_labels) mon_items.push_back(s.c_str());
    if (app.sm_kind != 0) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("##sm_mon", &app.sm_mon, mon_items.data(), static_cast<int>(mon_items.size()));
    if (app.sm_kind != 0) ImGui::EndDisabled();

    // 刺客 (assassin_type 0/1)
    v = (app.sm_kind == 1);
    if (ImGui::Checkbox("召唤刺客", &v)) app.sm_kind = v ? 1 : -1;
    ImGui::SameLine();
    std::vector<std::string> ass_labels;
    for (int i = 0; i < 2; ++i)
        ass_labels.push_back(std::to_string(i) + "：" + kSummonAssassinNames[i]);
    std::vector<const char*> ass_items;
    for (const auto& s : ass_labels) ass_items.push_back(s.c_str());
    if (app.sm_kind != 1) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("##sm_assassin", &app.sm_assassin, ass_items.data(),
                 static_cast<int>(ass_items.size()));
    if (app.sm_kind != 1) ImGui::EndDisabled();
    if (app.sm_kind == 1) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "攻击力较弱");
    }

    // 秦兵 (soldier_type 0-6) + 子类 (soldier_sub_type, 仅 1-6 时生效)
    v = (app.sm_kind == 2);
    if (ImGui::Checkbox("召唤秦兵", &v)) app.sm_kind = v ? 2 : -1;
    ImGui::SameLine();
    std::vector<std::string> sol_labels;
    sol_labels.push_back("0：随机秦兵（默认）");
    for (size_t i = 0; i < SummonQinCats().size(); ++i)
        sol_labels.push_back(std::to_string(i + 1) + "：" + SummonQinCats()[i].name);
    std::vector<const char*> sol_items;
    for (const auto& s : sol_labels) sol_items.push_back(s.c_str());
    if (app.sm_kind != 2) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    int old_soldier = app.sm_soldier;
    if (ImGui::Combo("##sm_soldier", &app.sm_soldier, sol_items.data(),
                     static_cast<int>(sol_items.size()))) {
        if (app.sm_soldier != old_soldier) {
            // 重填子类下拉(仅 1-6):过滤"旧重剑",子类 userData = 原列表索引
            app.sm_sub_items.clear();
            app.sm_sub = -1;
            if (app.sm_soldier >= 1 && app.sm_soldier <= 6) {
                const SummonCatUi& cat =
                    SummonQinCats()[static_cast<size_t>(app.sm_soldier - 1)];
                for (size_t i = 0; i < cat.subs.size(); ++i) {
                    if (app.sm_soldier == 2 && std::string(cat.subs[i]) == "旧重剑") continue;
                    app.sm_sub_items.push_back(static_cast<int>(i));
                }
                if (!app.sm_sub_items.empty()) app.sm_sub = app.sm_sub_items[0];
            }
        }
    }
    if (app.sm_kind != 2) ImGui::EndDisabled();
    // 子类下拉(仅 soldier_type∈1-6 且非空时 enable,对齐 task7 #3.3.1)
    ImGui::SameLine();
    bool sub_en = (app.sm_kind == 2 && app.sm_soldier >= 1 && app.sm_soldier <= 6 &&
                   !app.sm_sub_items.empty());
    std::vector<const char*> sub_items;
    int sub_sel = -1;
    if (sub_en) {
        const SummonCatUi& cat = SummonQinCats()[static_cast<size_t>(app.sm_soldier - 1)];
        for (size_t k = 0; k < app.sm_sub_items.size(); ++k) {
            sub_items.push_back(cat.subs[static_cast<size_t>(app.sm_sub_items[k])]);
            if (app.sm_sub_items[k] == app.sm_sub) sub_sel = static_cast<int>(k);
        }
        if (sub_sel < 0) sub_sel = 0;
    }
    if (!sub_en) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(160);
    if (ImGui::Combo("##sm_sub", &sub_sel, sub_items.data(),
                     static_cast<int>(sub_items.size())) &&
        sub_sel >= 0 && sub_sel < static_cast<int>(app.sm_sub_items.size())) {
        app.sm_sub = app.sm_sub_items[static_cast<size_t>(sub_sel)];
    }
    if (!sub_en) ImGui::EndDisabled();
}

// v1.30 功能区(task7 #4):mon_type 0-7 + 随机秦兵 checkbox(无子类下拉)。
void RenderSummon130Func(App& app) {
    SummonGroupRow(app);

    // 召唤野兽 (mon_type 0-7; 8朱厌/9善翼 不显示,映射保留在工具模块)
    bool v = (app.sm130_kind == 0);
    if (ImGui::Checkbox("召唤野兽", &v)) app.sm130_kind = v ? 0 : -1;
    ImGui::SameLine();
    std::vector<std::string> mon_labels = MakeMonLabels(kSummonMonNames, 8);
    std::vector<const char*> mon_items;
    for (const auto& s : mon_labels) mon_items.push_back(s.c_str());
    if (app.sm130_kind != 0) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("##sm130_mon", &app.sm130_mon, mon_items.data(),
                 static_cast<int>(mon_items.size()));
    if (app.sm130_kind != 0) ImGui::EndDisabled();

    // 随机秦兵 (仅 checkbox, 无下拉框; 对应 soldier_type=0, 对齐 task7 #4)
    v = (app.sm130_kind == 1);
    if (ImGui::Checkbox("随机秦兵", &v)) app.sm130_kind = v ? 1 : -1;
}

// 召唤修改 tab 主渲染。
void RenderSummonTab(App& app) {
    // 版本切换 -> 重置激活态(对齐 python reload_mode)
    if (app.summon_mode_cache != app.cfg.current_mode) {
        app.summon_mode_cache = app.cfg.current_mode;
        app.summon_active = false;
        app.summon_log.clear();
        app.sm_kind = 0;
        app.sm130_kind = 0;
        app.sm_sub = -1;
        app.sm_sub_items.clear();
    }

    // 第一行:激活状态标签(左) + 激活/取消 + 刷新召唤(右)
    // 激活时:"激活状态：激活，当前召唤的怪物：<类别>，<阵营>";阵营=队友(group 0)/盟友(group 1)
    std::string status_text = app.summon_active
                                  ? "激活状态：激活，当前召唤的怪物：" + SummonCurrentSelection(app) +
                                        "，" + (app.sm_group == 0 ? "队友" : "盟友")
                                  : "激活状态：默认：召唤狼";
    const float row_w = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float btn_w = 90.0f;
    const float refresh_w = 100.0f;
    const float label_w = ImGui::CalcTextSize(status_text.c_str()).x;
    ImGui::TextUnformatted(status_text.c_str());
    if (label_w + btn_w + refresh_w + spacing * 2.0f <= row_w)
        ImGui::SameLine(row_w - btn_w - refresh_w - spacing);

    // 激活/取消(不依赖初始化:未初始化也可点,点击时在 RunSummonTool 检查进程/版本)
    bool act_click = ImGui::Button(app.summon_active ? "取消" : "激活", ImVec2(btn_w, 0));
    ImGui::SameLine();
    if (!app.summon_active) ImGui::BeginDisabled();
    if (ImGui::Button("刷新召唤", ImVec2(refresh_w, 0))) OnSummonRefresh(app);
    if (!app.summon_active) ImGui::EndDisabled();
    if (act_click) OnSummonActivate(app);

    ImGui::Separator();

    // 第三行:功能区(steam / v1.30 各自独立,整组切换渲染)
    if (app.cfg.current_mode == "mode1")
        RenderSummonSteamFunc(app);
    else
        RenderSummon130Func(app);

    // F12 调试模式:显示工具输出日志(对齐 python set_log_visible, 只读可滚动,可复制/清空)
    if (!app.hidden && !app.summon_log.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("工具输出:");
        ImGui::SameLine();
        if (ImGui::Button("复制")) ImGui::SetClipboardText(app.summon_log.c_str());
        ImGui::SameLine();
        if (ImGui::Button("清空")) app.summon_log.clear();
        ImGui::BeginChild("summon_log", ImVec2(-FLT_MIN, 130), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(app.summon_log.c_str());
        ImGui::EndChild();
    }
}

// ============================================================================
// GM 命令 Tab(task8)
// ============================================================================
// 版本选择复用顶部全局选择器;steam/v1.30 的区别集中在 Core/GmCmd.h 的
// GmVersion 常量(对齐 python gm_cmd.py 的"单模块双版本参数化"),本文件只做
// UI 编排。纯 C++ 实现,不调用 python。

// 解析 gm_cmd.txt 文本 -> (选项列表, 格式错误行数),对齐 python
// gm_tab._parse_cmd_list:每行 "{命令}#{显示名}";空行忽略;行首 # 注释;
// 无 # 则命令=显示名;多个 # 取第一个分割;命令为空(格式不符) -> 计为错误行。
std::pair<std::vector<std::pair<std::string, std::string>>, int> ParseGmCmdText(
    const std::string& raw) {
    std::vector<std::pair<std::string, std::string>> items;
    int errors = 0;
    std::string cur;
    auto add_line = [&]() {
        std::string s = Trim(cur);
        cur.clear();
        if (s.empty() || s[0] == '#') return;
        std::string cmd = s, disp = s;
        size_t pos = s.find('#');
        if (pos != std::string::npos) {
            cmd = Trim(s.substr(0, pos));
            disp = Trim(s.substr(pos + 1));
            if (disp.empty()) disp = cmd;
        }
        if (!cmd.empty())
            items.emplace_back(cmd, disp);
        else
            ++errors;  // 命令为空(格式不符) -> 错误行
    };
    for (size_t i = 0; i <= raw.size(); ++i) {
        char c = (i < raw.size()) ? raw[i] : '\n';
        if (c == '\r') continue;
        if (c != '\n') {
            cur.push_back(c);
            continue;
        }
        add_line();
    }
    if (!cur.empty()) add_line();
    return {std::move(items), errors};
}

// 读取 gm_cmd.txt(工具目录,UTF-8)填充"常用命令"下拉,返回格式错误行数。
// keep_previous_on_error=true 且存在错误行时保留原列表不变(调用方弹窗提示);
// 否则用解析结果替换。文件不存在/不可读 -> 空表、0 错误(不报错)。
int LoadGmCmdList(App& app, bool keep_previous_on_error) {
    FILE* f = _wfopen((AppDirW() + L"\\gm_cmd.txt").c_str(), L"rb");
    if (!f) return 0;
    std::string raw;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);
    auto parsed = ParseGmCmdText(raw);
    if (parsed.second > 0 && keep_previous_on_error) return parsed.second;  // 保留旧列表
    app.gm_cmd_list = std::move(parsed.first);
    if (app.gm_cmdlist_sel >= static_cast<int>(app.gm_cmd_list.size()))
        app.gm_cmdlist_sel = 0;
    return parsed.second;
}

void AppendGmLog(App& app, const std::string& text) {
    if (text.empty()) return;
    if (!app.gm_log.empty()) app.gm_log += "\n";
    app.gm_log += text;
}

// 解析 GM 调用上下文(对齐 python gm_tab._call 的 pid/版本解析 + RunSummonTool 模式):
// pid 优先取已初始化(Modder 打开)的,否则取顶部下拉选中并临时 OpenProcess;
// 版本由当前模式决定(mode0=1.30 / mode1=steam),按该版本模块名找 rpg 基址,
// 找不到 -> 抛"版本不符"错误;exe = POQ.exe 基址。
void ResolveGmCtx(App& app, ProcessMemory& tmp_mem, ProcessMemory*& mem, unsigned int& pid,
                  uintptr_t& rpg, uintptr_t& exe, const gm_cmd::GmVersion*& ver) {
    mem = nullptr;
    if (app.modder.Mem().IsOpen()) {
        pid = app.modder.Pid();
        mem = &app.modder.Mem();
    } else {
        auto sel = TryGetSelectedPid(app);
        if (!sel)
            throw std::runtime_error("未选择进程：请先在顶部选择 POQ.EXE 并初始化。");
        pid = *sel;
        if (!tmp_mem.Open(pid))
            throw std::runtime_error("OpenProcess 失败（请以管理员身份运行）。");
        mem = &tmp_mem;
    }
    ver = (app.cfg.current_mode == "mode1") ? &gm_cmd::SteamVersion()
                                            : &gm_cmd::V130Version();
    auto base_opt = ProcessMemory::FindModuleBase(pid, ver->mod);
    if (!base_opt) {
        throw std::runtime_error(
            "未找到模块 " + ver->mod + "：当前运行的游戏版本与所选版本（" + app.cfg.Current().name +
            "）不符，或游戏未运行。请确认游戏正在运行，并在顶部选择正确的游戏版本后重新初始化。");
    }
    rpg = *base_opt;
    auto exe_opt = ProcessMemory::FindModuleBase(pid, "POQ.exe");
    exe = exe_opt.value_or(0);
}

// 静默刷新 GM 激活状态(对齐 python _refresh_status:读内存判断,不弹窗,失败=false)。
void GmRefreshReady(App& app) {
    bool ready = false;
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        ready = gm_cmd::IsGmReady(ctx);
    } catch (...) {
        ready = false;
    }
    app.gm_active = ready;
}

// GM 状态周期刷新(对齐 python QTimer 2s):仅 GM tab 可见时跑。
void TickGm(App& app, float dt) {
    if (!app.gm_visible) {
        app.gm_timer_accum = 0.0f;
        return;
    }
    app.gm_timer_accum += dt;
    if (app.gm_timer_accum < 2.0f) return;
    app.gm_timer_accum = 0.0f;
    GmRefreshReady(app);
}

void OnGmRestore(App& app);

void OnGmToggle(App& app) {
    if (app.gm_active) {
        OnGmRestore(app);
        return;
    }
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        gm_cmd::Method method =
            (app.gm_method == 1) ? gm_cmd::Method::R9 : gm_cmd::Method::Memory;
        // 激活/写 wizard.lst 的日志不进界面,进 F12 调试日志
        AppendGmLog(app, gm_cmd::Activate(ctx, method));
        GmRefreshReady(app);
        if (app.gm_active)
            app.SetStatus("GM 权限已激活。");
        else
            app.SetStatus("激活已执行，但未检测到 GM 权限（游戏内需读档进入后生效）。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("激活失败", msg, MB_ICONERROR);
        AppendGmLog(app, "激活失败：" + msg);
        app.SetStatus("激活失败：" + msg);
    }
}

void OnGmRestore(App& app) {
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        AppendGmLog(app, gm_cmd::Disable(ctx));
        GmRefreshReady(app);
        app.SetStatus("GM 权限已还原。");
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("还原失败", msg, MB_ICONERROR);
        AppendGmLog(app, "还原失败：" + msg);
        app.SetStatus("还原失败：" + msg);
    }
}

// 执行次数:未勾选=1;勾选则校验 1~20(对齐 python _parse_count)。非法返回 -1(已弹窗)。
int GmParseCount(App& app) {
    if (!app.ck_count) return 1;
    std::string txt = Trim(app.gm_count_buf);
    char* end = nullptr;
    long n = std::strtol(txt.c_str(), &end, 10);
    if (end == txt.c_str() || *end != '\0') {
        MsgBox("输入错误", "执行次数必须是整数。", MB_ICONWARNING);
        app.SetStatus("输入错误：执行次数必须是整数。");
        return -1;
    }
    if (n < 1 || n > 20) {
        MsgBox("输入错误", "执行次数必须在 1 ~ 20 之间。", MB_ICONWARNING);
        app.SetStatus("输入错误：执行次数必须在 1 ~ 20 之间。");
        return -1;
    }
    return static_cast<int>(n);
}

void OnGmExec(App& app) {
    std::string cmd = Trim(app.gm_cmd_buf);
    if (cmd.empty()) {
        MsgBox("命令为空", "请输入要执行的 GM 命令。", MB_ICONWARNING);
        app.SetStatus("命令为空");
        return;
    }
    int count = GmParseCount(app);
    if (count < 0) return;  // 已弹窗
    try {
        ProcessMemory tmp_mem;
        ProcessMemory* mem = nullptr;
        unsigned int pid = 0;
        uintptr_t rpg = 0, exe = 0;
        const gm_cmd::GmVersion* ver = nullptr;
        ResolveGmCtx(app, tmp_mem, mem, pid, rpg, exe, ver);
        gm_cmd::Ctx ctx{*mem, pid, rpg, exe, ver};
        for (int i = 0; i < count; ++i) {
            AppendGmLog(app, gm_cmd::Send(ctx, cmd));
            if (i + 1 < count) ::Sleep(100);  // 多次执行之间休息 100ms
        }
        // 记入本地命令历史(与上一条相同则不重复;最多缓存 30 条),浏览游标复位
        if (app.gm_history.empty() || app.gm_history.back() != cmd) {
            app.gm_history.push_back(cmd);
            if (app.gm_history.size() > 30) app.gm_history.erase(app.gm_history.begin());
        }
        app.gm_hist_idx = -1;
        // 执行命令的工具日志不进界面,进 F12 调试日志
        GmRefreshReady(app);
        app.SetStatus("已执行 " + std::to_string(count) + " 次命令：" + cmd);
    } catch (const std::exception& ex) {
        std::string msg = Sanitize(ex.what(), !app.hidden);
        MsgBox("执行失败", msg, MB_ICONERROR);
        AppendGmLog(app, "执行失败：" + msg);
        app.SetStatus("执行失败：" + msg);
    }
}

// GM命令 tab 主渲染(布局逐项对齐 python gm_tab.py _build_ui)。
void RenderGmTab(App& app) {
    app.gm_visible = true;
    // 版本切换 -> 重置激活态(对齐 python reload_mode)
    if (app.gm_mode_cache != app.cfg.current_mode) {
        app.gm_mode_cache = app.cfg.current_mode;
        app.gm_active = false;
        GmRefreshReady(app);
    }

    // 第1部分:激活状态行(左) + 激活方式 + 激活/还原(右)
    std::string status_text = app.gm_active ? "GM权限已激活" : "GM权限未激活";
    const ImVec4 status_color =
        app.gm_active ? ImVec4(0.0f, 0.8f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float row_w = ImGui::GetContentRegionAvail().x;
    // 三个按钮(激活/还原、执行、读取配置)统一宽度,统一右对齐
    const float gm_btn_w = ImGui::CalcTextSize("读取配置").x +
                           ImGui::GetStyle().FramePadding.x * 2.0f;
    const float method_w = 140.0f;
    const float method_label_w = ImGui::CalcTextSize("激活方式：").x;
    ImGui::TextColored(status_color, "%s", status_text.c_str());
    float right_w = method_label_w + method_w + gm_btn_w + spacing * 2.0f;
    if (ImGui::CalcTextSize(status_text.c_str()).x + right_w + spacing <= row_w)
        ImGui::SameLine(row_w - right_w);
    ImGui::TextUnformatted("激活方式：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(method_w);
    const char* method_items[] = {"内存修改", "R9注入"};
    ImGui::Combo("##gm_method", &app.gm_method, method_items, 2);
    ImGui::SameLine();
    bool act_click = ImGui::Button(app.gm_active ? "还原" : "激活", ImVec2(gm_btn_w, 0));
    if (act_click) OnGmToggle(app);
    ImGui::Separator();

    // 第3部分:命令输入 + 执行次数 + 执行(全部一行)
    ImGui::TextUnformatted("命令：");
    ImGui::SameLine();
    // 右侧固定部分:执行次数checkbox + 次数输入框 + 执行按钮(与激活/读取配置同宽),整体右对齐
    const float ck_total = ImGui::GetFrameHeight() + spacing + ImGui::CalcTextSize("执行次数").x;
    const float count_w = 80.0f;
    const float right_fixed = ck_total + count_w + gm_btn_w + spacing * 2.0f;
    float cmd_w = ImGui::GetContentRegionAvail().x - right_fixed - spacing;
    if (cmd_w < 150.0f) cmd_w = 150.0f;
    ImGui::SetNextItemWidth(cmd_w);
    if (!app.gm_active) ImGui::BeginDisabled();
    bool exec_pressed = ImGui::InputText("##gm_cmd", app.gm_cmd_buf, sizeof(app.gm_cmd_buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(row_w - right_fixed);  // 右对齐:执行次数/次数框/执行按钮贴右缘
    bool ck = app.ck_count;
    if (ImGui::Checkbox("执行次数", &ck)) app.ck_count = ck;
    ImGui::SameLine();
    bool count_en = app.gm_active && app.ck_count;
    if (!count_en) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputText("##gm_count", app.gm_count_buf, sizeof(app.gm_count_buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        exec_pressed = true;
    if (!count_en) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("执行", ImVec2(gm_btn_w, 0))) exec_pressed = true;
    if (!app.gm_active) ImGui::EndDisabled();
    if (exec_pressed) OnGmExec(app);

    // 第3.5部分:常用命令(一行:"常用命令：" 标签 + 右侧"读取配置"按钮;
    // 下方展开全部选项,点选后填入命令输入框)
    ImGui::TextUnformatted("常用命令：");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - gm_btn_w);
    if (ImGui::Button("读取配置", ImVec2(gm_btn_w, 0))) {
        // 重新读取 gm_cmd.txt;有错误行 -> 弹窗提示并保留上一次的选项
        int err = LoadGmCmdList(app, /*keep_previous_on_error=*/true);
        if (err > 0) {
            std::string msg = "常用命令文件 gm_cmd.txt 有 " + std::to_string(err) +
                              " 行格式错误，已保留上一次的选项。";
            MsgBox("读取配置", msg, MB_ICONWARNING);
            AppendGmLog(app, "读取配置：" + msg);
        } else {
            app.SetStatus("已重新读取常用命令 (" + std::to_string(app.gm_cmd_list.size()) +
                          " 条)");
            AppendGmLog(app, "读取配置：已重新读取 " +
                                 std::to_string(app.gm_cmd_list.size()) + " 条常用命令");
        }
    }
    // 下方:展开全部选项(自动限高,超出滚动);点选 -> 命令输入框显示对应命令
    app.gm_cmdlist_focused = false;
    if (app.gm_cmd_list.empty()) {
        ImGui::TextDisabled("（暂无常用命令：可编辑同目录 gm_cmd.txt 后点“读取配置”加载）");
    } else {
        // 占满下方剩余空间;下方还要画 F12 调试日志时预留其高度
        float list_h = ImGui::GetContentRegionAvail().y;
        if (!app.hidden && !app.gm_log.empty())
            list_h -= (ImGui::GetTextLineHeightWithSpacing() + 130.0f + spacing * 3.0f + 2.0f);
        if (list_h < 30.0f) list_h = 30.0f;
        if (!app.gm_active) ImGui::BeginDisabled();
        ImGui::BeginChild("##gm_cmdlist", ImVec2(ImGui::GetContentRegionAvail().x, list_h),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        app.gm_cmdlist_focused = ImGui::IsWindowFocused();  // 列表持焦点时箭头留给列表滚动
        for (int i = 0; i < static_cast<int>(app.gm_cmd_list.size()); ++i) {
            const auto& kv = app.gm_cmd_list[static_cast<size_t>(i)];
            bool selected = (i == app.gm_cmdlist_sel);
            if (ImGui::Selectable(kv.second.c_str(), selected)) {
                app.gm_cmdlist_sel = i;
                std::snprintf(app.gm_cmd_buf, sizeof(app.gm_cmd_buf), "%s", kv.first.c_str());
            }
        }
        ImGui::EndChild();
        if (!app.gm_active) ImGui::EndDisabled();
    }

    // 上下箭头切换历史命令(仅当常用命令列表未持有焦点;单行输入框不消费上下箭头)。
    // 上:从最新一条往前翻,到第一条失效;下:往回翻,翻过最新一条后还原浏览前的输入原文,
    // 再按失效。
    if (!app.gm_cmdlist_focused && !app.gm_history.empty()) {
        const int n = static_cast<int>(app.gm_history.size());
        int idx = app.gm_hist_idx;
        std::string new_text;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            if (idx == -1) {
                app.gm_hist_draft = app.gm_cmd_buf;  // 保存输入框原文
                idx = n - 1;                         // 从最新一条开始浏览
            } else if (idx > 0) {
                --idx;  // 到第一条(0)时不再上翻
            }
        } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            if (idx >= 0) {
                if (idx < n - 1) {
                    ++idx;  // 往新的方向翻
                } else {
                    idx = -1;  // 已在最新一条:下翻回到输入框原文,再按失效
                }
            }
        }
        if (idx != app.gm_hist_idx) {
            app.gm_hist_idx = idx;
            const std::string& text = (idx == -1) ? app.gm_hist_draft
                                                  : app.gm_history[static_cast<size_t>(idx)];
            std::snprintf(app.gm_cmd_buf, sizeof(app.gm_cmd_buf), "%s", text.c_str());
        }
    }

    // F12 调试模式:显示工具日志(对齐 summon tab 模式)
    if (!app.hidden && !app.gm_log.empty()) {
        ImGui::Separator();
        ImGui::TextUnformatted("工具输出:");
        ImGui::SameLine();
        if (ImGui::Button("复制##gmlog")) ImGui::SetClipboardText(app.gm_log.c_str());
        ImGui::SameLine();
        if (ImGui::Button("清空##gmlog")) app.gm_log.clear();
        ImGui::BeginChild("gm_log", ImVec2(-FLT_MIN, 130), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(app.gm_log.c_str());
        ImGui::EndChild();
    }
}

// ============================================================================
// GAM修改(task13): .gam 存档物品解析/修改/无中生有。
//   记录帧 [len][content][count][echo=len+12]; 字段相对"别名尾 aend"固定偏移;
//   词条=G_Info 字符串字段(GBK 文本); 地图=顶层容器(场景/物体组), 物品按区间归属。
//   全部文件级, 不依赖游戏注入。
// ============================================================================

static std::string GbkToUtf8Str(const std::string& g) {
    // 词条文本固定为 GBK(936), 不依赖系统区域(CP_ACP 在非中文系统会转乱码)
    if (g.empty()) return "";
    int n = ::MultiByteToWideChar(936, 0, g.data(), (int)g.size(), nullptr, 0);
    if (n <= 0) return g;
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(936, 0, g.data(), (int)g.size(), &w[0], n);
    return WideToUtf8(w);
}
static std::string Utf8ToGbkStr(const std::string& u) {
    if (u.empty()) return "";
    std::wstring w = Utf8ToWide(u);
    if (w.empty()) return u;
    int n = ::WideCharToMultiByte(936, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return u;
    std::string g((size_t)n, '\0');
    ::WideCharToMultiByte(936, 0, w.data(), (int)w.size(), &g[0], n, nullptr, nullptr);
    return g;
}

// 道具修改属性名(UTF-8) -> 该属性在 G_Info 里的词条名(UTF-8)。差异:
// "普通攻击"↔"普通攻击力"; "X属性攻击"↔"X系攻击力"; "普通防御"↔"普通防御力"; "X属性防御"↔"X系防御力"。
static std::string AttrGInfoAliasU8(const std::string& n) {
    return poq::ItemData::AttrGInfoName(n);   // 唯一映射表在 ItemData(GAM 写盘也用它)
}

// "29：普通攻击"(AttrDisplay 带 id 前缀+全角冒号) -> 纯名 "普通攻击"
static std::string AttrBareNameU8(const std::string& disp) {
    size_t p = disp.find("：");  // 全角冒号(UTF-8 3B)
    if (p == std::string::npos) return disp;
    return disp.substr(p + 3);
}

// GAM 字段名 -> "道具修改"字段标签(借其类型/枚举/范围做下拉与合法校验)
static const char* GamFieldToItemLabel(const std::string& name) {
    static const std::map<std::string, const char*> m = {
        {"lvl", "等级"},       {"dur", "耐久度"},     {"durmax", "耐久度上限"},
        {"price", "基准价格"}, {"wgt", "重量"},       {"type", "类型"},
        {"ident", "是否鉴定"}, {"sockets", "孔数"},   {"req_lvl", "等级需求"},
        {"req_str", "力量需求"}, {"req_agi", "敏捷需求"}, {"req_con", "体质需求"},
        {"wx", "五行"},        {"arrows", "弓箭数量"},
    };
    auto it = m.find(name);
    return it == m.end() ? nullptr : it->second;
}

// 前置声明(GamOnSave 会调用这些后定义的函数)
static int GamSelMode(const App& app);
static void ApplyGamPasteAttr(App& app, int idx, bool withIdent);   // 粘贴属性(先问后做)
static void GamCommitGInfo(App& app, poq::GamItem& it);

// 物品显示名: 中文名(别名), 没有中文映射时仅别名。
static std::string GamItemDispName(const App& app, const std::string& alias) {
    if (app.gam_item_names_loaded) {
        std::string cn = app.gam_item_names.Cn(alias);
        if (!cn.empty()) return cn + "(" + alias + ")";
    }
    return alias;
}

// 生成物品属性提示(悬停显示): 解析记录 -> 固定字段 + 词条文本。
static std::string GamFormatItemTip(const App& app, const std::vector<uint8_t>& rec) {
    poq::GamItem it;
    if (!poq::GamItem::ParseRecord(rec, it)) return "";
    std::string out = GamItemDispName(app, it.alias);
    bool ok = false;
    int32_t lvl = it.GetField("lvl", &ok);
    if (ok) out += "  Lv" + std::to_string((int)lvl);
    out += "\n";
    // 关键字段: 价格/重量/类型/鉴定/孔数/五行/需求/攻防
    auto addf = [&](const char* label, const char* name) {
        bool o = false;
        int32_t v = it.GetField(name, &o);
        if (o && v != -1) out += std::string(label) + std::to_string((int)v) + " ";
    };
    addf("价格", "price"); addf("重量", "wgt"); addf("类型", "type");
    addf("鉴定", "ident"); addf("孔数", "sockets"); addf("五行", "wx");
    addf("需等级", "req_lvl"); addf("需力量", "req_str");
    addf("需敏捷", "req_agi"); addf("需体质", "req_con");
    addf("攻击", "f_118"); addf("防御", "f_11c");
    out += "\n词条:\n" + GbkToUtf8Str(it.GetGInfo());
    return out;
}

// 持久化"上次打开 gam 的目录": 放 exe 同级 poqeditor_gam.ini(UTF-8 无 BOM 单行路径)
static std::wstring GamPrefsPath() {
    return AppDirW() + L"\\poqeditor_gam.ini";
}

static void GamSavePrefs(const std::string& last_dir) {
    FILE* fp = _wfopen(GamPrefsPath().c_str(), L"wb");
    if (!fp) return;
    fwrite(last_dir.data(), 1, last_dir.size(), fp);
    fclose(fp);
}

static std::string GamLoadPrefs() {
    FILE* fp = _wfopen(GamPrefsPath().c_str(), L"rb");
    if (!fp) return "";
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    std::string s(buf, n);
    size_t pos = s.find_first_of("\r\n");
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s;
}

// 时间戳备份路径: <dir>/<base>.<yyyymmddhhMMss>.bak.<ext> (后缀保持原扩展名)
static std::wstring TimestampedBakPath(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    std::wstring fn = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    size_t dot = fn.find_last_of(L'.');
    std::wstring base = (dot == std::wstring::npos) ? fn : fn.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos) ? L"" : fn.substr(dot);
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t ts[32];
    ::swprintf(ts, 32, L"%04d%02d%02d%02d%02d%02d", (int)st.wYear, (int)st.wMonth,
               (int)st.wDay, (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    return dir + base + L"." + ts + L".bak" + ext;
}

static std::wstring GamInitialDir(App& app) {
    // 游戏运行时优先定位到其存档目录 System\game
    std::optional<unsigned int> pid;
    if (app.modder.Mem().IsOpen())
        pid = app.modder.Pid();
    else if (auto s = TryGetSelectedPid(app))
        pid = s;
    if (!pid) pid = FindPoqPid(app);
    if (pid) {
        auto path = poq::ProcessMemory::FindModulePath(*pid, "POQ.exe");
        if (path) {
            std::string p = *path;
            size_t pos = p.find_last_of("/\\");
            if (pos != std::string::npos) {
                std::string exedir = p.substr(0, pos);
                std::wstring sdir = Utf8ToWide(exedir + "\\System\\game");
                if (::GetFileAttributesW(sdir.c_str()) != INVALID_FILE_ATTRIBUTES) return sdir;
                std::wstring dir = Utf8ToWide(exedir);
                if (!dir.empty() && ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return dir;
            }
        }
    }
    // 游戏未运行:记忆上次打开目录
    if (!app.gam_last_dir.empty()) {
        std::wstring w = Utf8ToWide(app.gam_last_dir);
        if (::GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) return w;
    }
    return AppDirW();
}

static void GamOnOpen(App& app) {
    std::string sel =
        OpenFileDialog(GamInitialDir(app), L"GAM 存档 (*.gam)\0*.gam\0所有文件 (*)\0*.*\0\0");
    if (sel.empty()) return;
    poq::GamFile gf;
    std::string err;
    if (!poq::GamFile::Load(sel, gf, err)) {
        MsgBox("打开失败", err, MB_ICONERROR);
        return;
    }
    // 打开时自动备份原文件: <base>.<yyyymmddhhMMss>.bak.gam
    {
        std::wstring w = Utf8ToWide(sel);
        std::wstring bak = TimestampedBakPath(w);
        if (::CopyFileW(w.c_str(), bak.c_str(), FALSE)) TrackBackup(w, bak);
    }
    app.gam_file = std::move(gf);
    app.gam_path = sel;
    app.gam_loaded = true;
    app.gam_sel_container = -1;
    app.gam_sel_item = -1;
    app.gam_edit_loaded_item = -1;
    app.gam_ginfo_pending = false;
    // 记忆上次打开目录(下次打开gam默认进该目录, 持久化到 exe 同级 ini)
    {
        size_t pos = sel.find_last_of("/\\");
        if (pos != std::string::npos) app.gam_last_dir = sel.substr(0, pos);
    }
    GamSavePrefs(app.gam_last_dir);
    app.SetStatus("GAM 已打开: " + app.gam_path + "（容器 " +
                  std::to_string(app.gam_file.containers.size()) + "，物品 " +
                  std::to_string(app.gam_file.items.size()) + "）");
}

static void GamOnSave(App& app) {
    if (!app.gam_loaded) {
        MsgBox("提示", "请先打开 gam 存档。", MB_ICONINFORMATION);
        return;
    }
    // 提交当前物品的词条编辑(装配回 G_Info)
    if (GamSelMode(app) == 1)
        GamCommitGInfo(app, app.gam_file.items[(size_t)app.gam_sel_item]);
    // 组装待插入记录(所有 is_new 物品 -> 按其当前内容重编)
    std::vector<std::pair<int, std::vector<uint8_t>>> added;
    for (size_t i = 0; i < app.gam_file.items.size(); ++i) {
        const auto& it = app.gam_file.items[i];
        if (!it.is_new) continue;
        int ci = -1;
        for (size_t c = 0; c < app.gam_file.containers.size(); ++c)
            for (int idx : app.gam_file.containers[c].item_idx)
                if (idx == (int)i) { ci = (int)c; break; }
        if (ci >= 0) {
            std::vector<uint8_t> rec;
            if (poq::GamItem::BuildRecord(it, rec)) added.push_back({ci, std::move(rec)});
        }
    }
    std::string err;
    if (!poq::GamFile::Save(app.gam_file, app.gam_path, added, err)) {
        MsgBox("保存失败", err, MB_ICONERROR);
        return;
    }
    app.SetStatus("GAM 已保存：" + app.gam_path + "（新增 " + std::to_string(added.size()) + " 件）");
    // 重载同步内存(位置/偏移已整体后移)
    poq::GamFile gf;
    std::string e;
    if (poq::GamFile::Load(app.gam_path, gf, e)) {
        int keep_c = app.gam_sel_container, keep_i = app.gam_sel_item;
        app.gam_file = std::move(gf);
        app.gam_sel_container = keep_c;
        app.gam_sel_item = keep_i;
        app.gam_edit_loaded_item = -1;
    }
}

// 非装备记录(技能等): 词条数组里含"装备属性表(0x00..0x62)之外"的 id(技能属性在 0x10x/0x11x)。
// 这类记录的词条名/id 不在下拉表里, 编辑会把它们从词条数组里丢掉 -> 禁用属性编辑。
static bool GamIsNonEquipment(const poq::GamItem& it) {
    if (!it.has_affixes) return false;
    for (const auto& a : it.affixes)
        if (poq::ItemData::AttrNames().find((uint32_t)a.id) == poq::ItemData::AttrNames().end())
            return true;
    return false;
}

static int GamSelMode(const App& app) {
    if (app.gam_sel_container >= 0 && app.gam_sel_item >= 0 &&
        app.gam_sel_item < (int)app.gam_file.items.size())
        return 1;  // 选中物品
    if (app.gam_sel_container >= 0 && app.gam_sel_container < (int)app.gam_file.containers.size())
        return 2;  // 仅选中地图
    return 0;
}

// 复制指定物品的整条记录到剪贴板。
static void GamCopyItemAt(App& app, int idx) {
    if (idx < 0 || idx >= (int)app.gam_file.items.size()) return;
    const poq::GamItem& it = app.gam_file.items[(size_t)idx];
    if (it.is_new) {
        poq::GamItem::BuildRecord(it, app.gam_clip_record);
    } else if (it.rec_end <= app.gam_file.buf.size()) {
        app.gam_clip_record.assign(app.gam_file.buf.begin() + (ptrdiff_t)it.pos,
                                   app.gam_file.buf.begin() + (ptrdiff_t)it.rec_end);
    }
    app.gam_has_record = !app.gam_clip_record.empty();
    app.SetStatus(app.gam_has_record ? "已复制物品：" + it.alias : "复制物品失败");
}

static void GamOnCopyItem(App& app) {
    if (GamSelMode(app) != 1) {
        MsgBox("提示", "请先在左侧选中一件物品，再点\"复制物品\"。", MB_ICONINFORMATION);
        return;
    }
    GamCopyItemAt(app, app.gam_sel_item);
}

// 把新建物品放到目标容器已有物品右侧(错开坐标, 避免叠位/不可见; 无已有物品则保留源坐标)
static void GamPlaceNewItem(App& app, int item_idx) {
    if (item_idx < 0 || item_idx >= (int)app.gam_file.items.size()) return;
    if (app.gam_sel_container < 0 ||
        app.gam_sel_container >= (int)app.gam_file.containers.size())
        return;
    auto& cont = app.gam_file.containers[(size_t)app.gam_sel_container];
    long maxx = -0x7FFFFFFF;
    for (int idx : cont.item_idx) {
        if (idx < 0 || idx >= (int)app.gam_file.items.size()) continue;
        if (idx == item_idx) continue;
        bool ok = false;
        int32_t x = app.gam_file.items[(size_t)idx].GetField("f_c8", &ok);
        if (ok && x >= 0 && x < 0x1000000) maxx = std::max<long>(maxx, (long)x);
    }
    int newx = (maxx > -0x7FFFFFFF) ? (int)(maxx + 40) : 100;
    app.gam_file.items[(size_t)item_idx].SetField("f_c8", newx);
}

// 把剪贴板记录作为新物品粘贴到容器 ci。
static void GamPasteInto(App& app, int ci) {
    if (!app.gam_has_record || app.gam_clip_record.empty()) {
        MsgBox("提示", "剪贴板中没有物品记录，请先\"复制物品\"或从目录\"创建物品\"。", MB_ICONINFORMATION);
        return;
    }
    if (ci < 0 || ci >= (int)app.gam_file.containers.size()) return;
    poq::GamItem it;
    if (!poq::GamItem::ParseRecord(app.gam_clip_record, it)) {
        MsgBox("粘贴失败", "剪贴板记录不是合法物品帧。", MB_ICONERROR);
        return;
    }
    it.is_new = true;
    auto& items = app.gam_file.items;
    int idx = (int)items.size();
    items.push_back(std::move(it));
    app.gam_file.containers[(size_t)ci].item_idx.push_back(idx);
    app.gam_sel_container = ci;
    app.gam_sel_item = idx;
    app.gam_edit_loaded_item = -1;
    GamPlaceNewItem(app, idx);  // 错开坐标避免叠位/不可见
    app.SetStatus("已粘贴物品：" + items[(size_t)idx].alias);
}

static void GamOnPasteItem(App& app) {
    if (GamSelMode(app) < 2) {
        MsgBox("提示", "请先在左侧选中一个地图(容器)。", MB_ICONINFORMATION);
        return;
    }
    GamPasteInto(app, app.gam_sel_container);
}

static void GamOnCopyAttr(App& app) {
    if (GamSelMode(app) != 1) {
        MsgBox("提示", "请先选中一件物品(在右侧编辑界面)。", MB_ICONINFORMATION);
        return;
    }
    const poq::GamItem& it = app.gam_file.items[(size_t)app.gam_sel_item];
    if (GamIsNonEquipment(it)) {
        MsgBox("提示", "这是非装备记录（技能等），不支持复制属性。", MB_ICONINFORMATION);
        return;
    }
    app.gam_clip_fields.clear();
    for (const auto& f : poq::GamFile::FieldList()) {
        bool ok = false;
        int32_t v = it.GetField(f.name, &ok);
        if (ok) app.gam_clip_fields[f.name] = v;
    }
    app.gam_clip_ginfo = it.GetGInfo();
    app.gam_clip_name = GbkToUtf8Str(it.name);   // 名字(UTF-8, 供询问框显示)
    app.gam_clip_alias = it.alias;
    app.gam_has_attrs = true;
    app.SetStatus("已复制属性：" + it.alias);
}

static void GamOnPasteAttr(App& app) {
    if (GamSelMode(app) != 1) {
        MsgBox("提示", "请先选中一件物品(在右侧编辑界面)。", MB_ICONINFORMATION);
        return;
    }
    if (!app.gam_has_attrs) {
        MsgBox("提示", "剪贴板中没有属性，请先\"复制属性\"。", MB_ICONINFORMATION);
        return;
    }
    poq::GamItem& it = app.gam_file.items[(size_t)app.gam_sel_item];
    if (GamIsNonEquipment(it)) {
        MsgBox("提示", "这是非装备记录（技能等），属性编辑已禁用，不能粘贴属性。", MB_ICONINFORMATION);
        return;
    }
    // 先问"名字/别名/类型"要不要一起粘贴, 再动手(取消 = 什么都不做)
    int idx = app.gam_sel_item;
    std::string q = "来源物品:";
    if (!app.gam_clip_name.empty()) q += "\n  名字: " + app.gam_clip_name;
    if (!app.gam_clip_alias.empty()) q += "\n  别名: " + app.gam_clip_alias;
    {
        auto ft = app.gam_clip_fields.find("type");
        int tv = ft != app.gam_clip_fields.end() ? (int)ft->second : -1;
        q += "\n  类型: " + FieldValueDisplay(app, "类型", tv);   // 枚举显示"值：名字"
    }
    q += "\n\n这三项(名字/别名/类型)要不要一起粘贴？其余属性按所选方式粘贴。";
    App* ap = &app;
    MsgBoxAsk("粘贴属性", q, [ap, idx](int choice) {
        if (choice == 2) {
            ap->SetStatus("已取消粘贴属性(什么都没改)。");
            return;
        }
        ApplyGamPasteAttr(*ap, idx, choice == 0);
    });
}

// 真正做粘贴: withIdent=true 连名字/别名/类型一起; false 只粘贴其余属性。
static void ApplyGamPasteAttr(App& app, int idx, bool withIdent) {
    if (idx < 0 || idx >= (int)app.gam_file.items.size()) return;
    poq::GamItem& it = app.gam_file.items[(size_t)idx];
    bool ginfo_ok = true;
    for (const auto& kv : app.gam_clip_fields) {
        if (!withIdent && kv.first == "type") continue;  // 类型归入"名字/别名"一档
        it.SetField(kv.first.c_str(), kv.second);
    }
    if (!app.gam_clip_ginfo.empty()) ginfo_ok = it.SetGInfo(app.gam_clip_ginfo);
    if (withIdent) {
        if (!app.gam_clip_name.empty()) it.SetName(Utf8ToGbkStr(app.gam_clip_name));
        if (!app.gam_clip_alias.empty()) it.SetAlias(app.gam_clip_alias);
    }
    app.gam_edit_loaded_item = -1;  // 刷新界面
    std::string msg = std::string(withIdent ? "已粘贴属性+名字/别名/类型到 " : "已粘贴属性(名字/别名/类型保持原样)到 ") +
                      it.alias;
    if (!ginfo_ok) msg += "（词条文本无法原位覆盖，已保留原词条）";
    msg += "。记得点\"保存文件\"。";
    app.SetStatus(msg);
}

static void GamOnCreate(App& app) {
    if (!app.gam_cat_loaded || app.gam_sel_cat < 0 ||
        app.gam_sel_cat >= (int)app.gam_catalog.cats.size()) {
        MsgBox("提示", "物品目录未加载（缺少 gam_catalog.bin，请放 exe 目录或 data 子目录）。",
               MB_ICONINFORMATION);
        return;
    }
    const auto& ents = app.gam_catalog.items[(size_t)app.gam_sel_cat];
    if (ents.empty() || app.gam_sel_cat_item < 0 ||
        app.gam_sel_cat_item >= (int)ents.size())
        return;
    if (GamSelMode(app) < 2) {
        MsgBox("提示", "请先在左侧选中一个地图(容器)。", MB_ICONINFORMATION);
        return;
    }
    const auto& e = ents[(size_t)app.gam_sel_cat_item];
    poq::GamItem it;
    if (!poq::GamItem::ParseRecord(e.rec, it)) {
        MsgBox("创建失败", "目录记录不是合法物品帧。", MB_ICONERROR);
        return;
    }
    it.is_new = true;
    auto& items = app.gam_file.items;
    int idx = (int)items.size();
    items.push_back(std::move(it));
    app.gam_file.containers[(size_t)app.gam_sel_container].item_idx.push_back(idx);
    app.gam_sel_item = idx;
    app.gam_edit_loaded_item = -1;
    GamPlaceNewItem(app, idx);  // 错开坐标避免叠位/不可见
    app.SetStatus("已创建物品：" + GamItemDispName(app, e.name));
}

static void RenderGamTree(App& app) {
    ImGui::BeginChild("gamtree", ImVec2(0.0f, ImGui::GetContentRegionAvail().y),
                      ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    static char filt[128] = {0};
    ImGui::TextUnformatted("地图(区域) -> 房间/物体 -> 物品：");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##gamfilt", filt, sizeof(filt));
    ImGui::Separator();
    ImGui::BeginChild("gamtree2");
    std::string lowf = ToLower(filt);

    auto seg = [](const std::string& path, int a, int b) {  // "/" 分割取 [a,b)
        std::vector<std::string> segs;
        size_t i = 0;
        while (true) {
            size_t j = path.find('/', i);
            if (j == std::string::npos) { segs.push_back(path.substr(i)); break; }
            segs.push_back(path.substr(i, j - i));
            i = j + 1;
        }
        std::string out;
        for (int k = a; k < b && k < (int)segs.size(); ++k) {
            if (!out.empty()) out += "/";
            out += segs[(size_t)k];
        }
        return out;
    };
    // 构建分级结构(P1: 缓存进 App, 物品数变化才重建 —— 世界档 558 容器每帧构建太浪费)
    auto& regions = app.gam_regions;
    if ((int)app.gam_file.items.size() != app.gam_regions_items) {
        app.gam_regions.clear();
        app.gam_regions_items = (int)app.gam_file.items.size();
        std::map<std::string, int> ridx;
        std::string cur_region;
        for (int ci = 0; ci < (int)app.gam_file.containers.size(); ++ci) {
            const auto& c = app.gam_file.containers[(size_t)ci];
            if (c.item_idx.empty()) continue;
            std::string path = app.gam_scene_loaded ? app.gam_scene_cat.MatchPath(c.alias) : "";
            std::string region_disp, room_label;
            bool is_scene = false;
            if (!path.empty()) {
                region_disp = seg(path, 0, 2);      // 秦殇/邯郸
                room_label = seg(path, 3, 4);       // 武器铺
                if (room_label.empty()) room_label = c.alias;
                cur_region = region_disp;
                is_scene = true;
            } else {
                room_label = c.alias;               // 物体(未匹配场景)
                region_disp = cur_region.empty() ? "未分类" : cur_region;
            }
            if (ridx.find(region_disp) == ridx.end()) {
                ridx[region_disp] = (int)app.gam_regions.size();
                app.gam_regions.push_back(
                    App::GamRegion{region_disp, ToLower(region_disp), {}, {}});
            }
            App::GamRoom r{room_label, ci, ToLower(room_label)};
            if (is_scene)
                app.gam_regions[(size_t)ridx[region_disp]].rooms.push_back(std::move(r));
            else
                app.gam_regions[(size_t)ridx[region_disp]].objs.push_back(std::move(r));
        }
    }

    // 渲染
    auto item_matches = [&](int idx) {
        if (idx < 0 || idx >= (int)app.gam_file.items.size()) return false;
        const auto& it = app.gam_file.items[(size_t)idx];
        if (it.removed) return false;
        if (lowf.empty()) return true;
        // P1: 装载时已把 alias/显示名转好小写缓存, 每帧零分配
        return it.alias_low.find(lowf) != std::string::npos ||
               it.disp_low.find(lowf) != std::string::npos;
    };
    auto cont_live_count = [&](int ci) {
        int n = 0;
        for (int idx : app.gam_file.containers[(size_t)ci].item_idx)
            if (idx >= 0 && (size_t)idx < app.gam_file.items.size() &&
                !app.gam_file.items[(size_t)idx].removed)
                ++n;
        return n;
    };
    auto render_items = [&](int ci) {
        const auto& c = app.gam_file.containers[(size_t)ci];
        for (int idx : c.item_idx) {
            if (idx < 0 || idx >= (int)app.gam_file.items.size()) continue;
            auto& it = app.gam_file.items[(size_t)idx];
            if (it.removed) continue;
            if (!item_matches(idx)) continue;
            bool ok = false;
            int32_t lvl = it.GetField("lvl", &ok);
            std::string disp = GamItemDispName(app, it.alias);
            if (disp.size() > 220) disp = disp.substr(0, 220);
            char buf[320];
            std::string tag;
            if (it.is_new) tag += " [新增]";
            if (GamIsNonEquipment(it)) tag += " [非装备记录]";
            std::snprintf(buf, sizeof(buf), "(%d) %s  Lv%d%s##i%s%zX", idx, disp.c_str(),
                          ok ? (int)lvl : 0, tag.c_str(),
                          it.is_new ? "n" : "", it.is_new ? (size_t)idx : it.pos);
            ImGui::PushID(idx);
            if (ImGui::SmallButton("删除")) {  // 删除该物品(内存标记,保存时生效)
                it.removed = true;
                if (app.gam_sel_item == idx) {
                    app.gam_sel_item = -1;
                    app.gam_edit_loaded_item = -1;
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("复制")) GamCopyItemAt(app, idx);  // 复制该物品记录
            ImGui::SameLine();
            bool sel = (app.gam_sel_item == idx);
            if (ImGui::Selectable(buf, sel)) {
                app.gam_sel_container = ci;
                app.gam_sel_item = idx;
                app.gam_edit_loaded_item = -1;
            }
            ImGui::PopID();
        }
    };
    auto render_group = [&](const App::GamRoom& room, const char* idpfx) {
        const auto& c = app.gam_file.containers[(size_t)room.cont];
        bool cont_sel = (app.gam_sel_container == room.cont && app.gam_sel_item < 0);
        char lab[320];
        std::snprintf(lab, sizeof(lab), "%s (%d)##%s%X", room.label.c_str(),
                      cont_live_count(room.cont), idpfx, (unsigned)(c.start & 0xFFFFFFFF));
        ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick;
        if (cont_sel) f |= ImGuiTreeNodeFlags_Selected;
        bool open = ImGui::TreeNodeEx(lab, f);
        if (ImGui::IsItemClicked()) {  // 点标签=选中该容器;点箭头=仅展开(OpenOnArrow)
            app.gam_sel_container = room.cont;
            app.gam_sel_item = -1;
        }
        ImGui::SameLine();
        ImGui::PushID(1000000 + room.cont);
        if (ImGui::SmallButton("删除")) {  // 删该容器下所有物品
            for (int idx : c.item_idx)
                if (idx >= 0 && (size_t)idx < app.gam_file.items.size())
                    app.gam_file.items[(size_t)idx].removed = true;
            app.gam_sel_container = room.cont;
            app.gam_sel_item = -1;
            app.gam_edit_loaded_item = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("粘贴")) GamPasteInto(app, room.cont);  // 粘贴剪贴板物品到该容器
        ImGui::PopID();
        if (open) {
            render_items(room.cont);
            ImGui::TreePop();
        }
    };
    for (int ri = 0; ri < (int)regions.size(); ++ri) {
        const App::GamRegion& R = regions[(size_t)ri];
        // 过滤: 区域名 / 任一房间名 / 任一物品名 命中才显示
        bool any = lowf.empty() || R.disp_low.find(lowf) != std::string::npos;
        if (!any) {
            for (const auto& rm : R.rooms) {
                if (ToLower(rm.label).find(lowf) != std::string::npos) { any = true; break; }
                for (int idx : app.gam_file.containers[(size_t)rm.cont].item_idx)
                    if (item_matches(idx)) { any = true; break; }
                if (any) break;
            }
        }
        if (!any) {
            for (const auto& ob : R.objs) {
                if (ToLower(ob.label).find(lowf) != std::string::npos) { any = true; break; }
                for (int idx : app.gam_file.containers[(size_t)ob.cont].item_idx)
                    if (item_matches(idx)) { any = true; break; }
                if (any) break;
            }
        }
        if (!any) continue;
        std::string rlabel = R.disp + "##r" + std::to_string(ri);
        if (!ImGui::TreeNodeEx(rlabel.c_str(),
                               ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }
        for (const auto& room : R.rooms) render_group(room, "room");
        for (const auto& obj : R.objs) render_group(obj, "obj");
        ImGui::TreePop();
    }
    ImGui::EndChild();
    ImGui::EndChild();
}

// 词条颜色 -> 中文标签
static const char* GamColorLabel(char c) {
    switch (c) {
        case 'K': return "蓝色";
        case 'C': return "亮蓝色(激活隐藏)";
        case 'Z': return "金色";
        case 'H': return "暗蓝色(未激活隐藏)";
        default:  return "白色";
    }
}
static const char kGamColorLetters[] = {0, 'K', 'C', 'Z', 'H'};

// 把当前物品的 G_Info 解析进编辑状态(隐藏头行, 只留词条)
static void GamLoadGInfoRows(App& app, poq::GamItem& it) {
    app.gam_grows.clear();
    it.ParseGInfoRows(app.gam_grows);
    app.gam_grow_sel.assign(app.gam_grows.size(), 0);   // 勾选状态随行数重置
    app.gam_gheader = it.GetGInfoHeader();
    // 记下"装载时的装配结果"。游戏写的文本有些行尾没空格, 而 BuildGInfoText 一律补一个,
    // 所以直接比对原文会误判成"改过"; 用装载时装配的结果做基准, 才能区分出真正的用户编辑。
    app.gam_gtext_loaded.clear();
    poq::GamItem::BuildGInfoText(app.gam_gheader, app.gam_grows, app.gam_gtext_loaded);
}

// 把编辑态的词条装配回 G_Info(等长/变长均可; 变长由保存时重排落盘)
static void GamCommitGInfo(App& app, poq::GamItem& it) {
    std::string txt;
    if (!poq::GamItem::BuildGInfoText(app.gam_gheader, app.gam_grows, txt)) return;
    if (txt == app.gam_gtext_loaded) return;   // 用户没动过词条: 不写, 免得把没改的物品标脏
    if (it.SetGInfo(txt)) app.gam_gtext_loaded = txt;
}

static void RenderGamEdit(App& app) {
    if (GamSelMode(app) != 1) {
        ImGui::TextDisabled("左侧选中地图后，点选一件物品进入编辑。");
        return;
    }
    poq::GamItem& it = app.gam_file.items[(size_t)app.gam_sel_item];
    // 非装备记录(技能等): 词条 id 不在装备属性表里, 编辑会破坏它 -> 直接不给改
    if (GamIsNonEquipment(it)) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "这是非装备记录（技能等），属性编辑已禁用。");
        ImGui::TextWrapped(
            "它的词条用的是另一套属性 id（0x10x/0x11x，如 驯兽最大数量 / 召唤伙伴个数），不在装备属性表内。"
            "编辑器无法安全改写它的词条数组，所以这里只读；需要删除的话在左侧树上用\"删除\"。");
        if (app.gam_loaded) {
            ImGui::Separator();
            ImGui::Text("记录: %s  长度 %d  词条节点 %d", it.alias.c_str(), (int)it.content.size(),
                        (int)it.affixes.size());
            for (const auto& a : it.affixes)
                ImGui::BulletText("id 0x%03X  值 %g  颜色 %u  激活 %u", (unsigned)a.id, a.value,
                                  a.color, a.active);
        }
        return;
    }
    // 选中物品变化: 先提交上一件的词条编辑, 再载入新物品
    if (app.gam_edit_loaded_item != app.gam_sel_item) {
        if (app.gam_edit_loaded_item >= 0 &&
            app.gam_edit_loaded_item < (int)app.gam_file.items.size())
            GamCommitGInfo(app, app.gam_file.items[(size_t)app.gam_edit_loaded_item]);
        GamLoadGInfoRows(app, it);
        std::snprintf(app.gam_alias_buf, sizeof(app.gam_alias_buf), "%s", it.alias.c_str());
        std::snprintf(app.gam_name_buf, sizeof(app.gam_name_buf), "%s",
                      GbkToUtf8Str(it.name).c_str());
        app.gam_edit_loaded_item = app.gam_sel_item;
    }
    // 名称(标签放输入框左边)
    ImGui::Text("记录长度 %u 字节%s", it.len, it.is_new ? "   （新增，保存后写入所选地图）" : "");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("道具名字");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::InputText("##gamname", app.gam_name_buf, sizeof(app.gam_name_buf))) {
        if (!it.SetName(Utf8ToGbkStr(app.gam_name_buf)))  // 非法则还原
            std::snprintf(app.gam_name_buf, sizeof(app.gam_name_buf), "%s",
                          GbkToUtf8Str(it.name).c_str());
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("道具别名");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputText("##gamalias", app.gam_alias_buf, sizeof(app.gam_alias_buf),
                         ImGuiInputTextFlags_CharsNoBlank)) {
        if (!it.SetAlias(app.gam_alias_buf))  // 非法则还原
            std::snprintf(app.gam_alias_buf, sizeof(app.gam_alias_buf), "%s", it.alias.c_str());
    }
    ImGui::Separator();
    // 基础字段网格(3 列)
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail >= 800.0f ? 4 : (avail >= 500.0f ? 3 : 2);
    if (ImGui::BeginTable("gambasic", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        const auto& bfs = app.cfg.Current().basic_fields;
        auto findCfg = [&](const char* label) -> const poq::BasicFieldCfg* {
            for (const auto& bf : bfs)
                if (bf.label == label) return &bf;
            return nullptr;
        };
        for (const auto& f : poq::GamFile::FieldList()) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(f.label);
            bool ok = false;
            int32_t v = it.GetField(f.name, &ok);
            int ed = ok ? (int)v : -1;
            const char* lbl = GamFieldToItemLabel(f.name);
            const poq::BasicFieldCfg* bf = lbl ? findCfg(lbl) : nullptr;
            std::string id = "##gam" + std::string(f.name);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (bf && !bf->enum_map.empty()) {
                // 枚举 -> 下拉框(与"道具修改"一致;enum_display_value 显示 "值：名字")
                std::vector<std::pair<int, std::string>> opts;
                for (const auto& kv : bf->enum_map)
                    opts.push_back({kv.first, bf->enum_display_value
                                                 ? std::to_string(kv.first) + "：" + kv.second
                                                 : kv.second});
                std::vector<const char*> items;
                for (auto& o : opts) items.push_back(o.second.c_str());
                int cur = -1;
                for (size_t j = 0; j < opts.size(); ++j)
                    if (opts[j].first == ed) { cur = (int)j; break; }
                if (ImGui::Combo(id.c_str(), &cur, items.data(), (int)items.size())) {
                    if (cur >= 0) it.SetField(f.name, opts[(size_t)cur].first);
                }
            } else if (ImGui::InputInt(id.c_str(), &ed)) {
                if (bf) ed = bf->Clamp(ed);  // 合法校验与"道具修改"一致
                it.SetField(f.name, ed);
            }
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    // 词条: 一行一个属性(隐藏模板头), 保存时装配回 G_Info
    ImGui::TextUnformatted("词条（一行一个属性，保存时装配）：");
    const char* color_items[] = {"白色", "蓝色(K)", "亮蓝色(C,激活隐藏)", "金色(Z)", "暗蓝色(H,未激活隐藏)"};
    const char color_letters[] = {0, 'K', 'C', 'Z', 'H'};
    if (ImGui::BeginTable("grows", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        // 列顺序与"道具修改"一致: 选择 / 属性名 / 属性颜色 / 类型 / 属性值
        ImGui::TableSetupColumn("选择", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableSetupColumn("属性名", ImGuiTableColumnFlags_WidthFixed, 160.0f);
        ImGui::TableSetupColumn("属性颜色", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("属性值", ImGuiTableColumnFlags_WidthStretch, 0.0f);
        ImGui::TableHeadersRow();
        // 属性名下拉(显示同"道具修改"如 "29：普通攻击"); 用纯名别名映射匹配 G_Info 名
        static std::vector<std::string> s_disp;
        static std::vector<std::string> s_bare;
        if (s_disp.empty()) {
            for (auto& d : ItemData::AttrDisplayList()) {
                s_disp.push_back(d);
                s_bare.push_back(AttrBareNameU8(d));
            }
            // 下拉框只列"道具修改"表里的属性名 —— 不再额外补 GAM 专有名字:
            //   异常状态时间减少/失明攻击几率/失明时间/失明抗性 是映射结果名, 列出来是重复项;
            //   召唤伙伴个数/驯兽最大数量 映射不到属性 id, 写盘时会被从词条数组里丢掉(选中也没用)。
            // 存档里遇到的这类名字, 下拉框会用"原名置顶"的方式显示出来, 读取不受影响。
        }
        static const std::vector<std::string>& pct_list = ItemData::PercentDisplayListRef();
        static std::vector<const char*> pct_c;
        if (pct_c.empty())
            for (auto& t : pct_list) pct_c.push_back(t.c_str());
        for (int i = 0; i < (int)app.gam_grows.size(); ++i) {
            auto& r = app.gam_grows[(size_t)i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            if (ImGui::TableSetColumnIndex(0)) {
                bool bsel = (i < (int)app.gam_grow_sel.size()) && app.gam_grow_sel[(size_t)i];
                if (ImGui::Checkbox("##sel", &bsel) && i < (int)app.gam_grow_sel.size())
                    app.gam_grow_sel[(size_t)i] = bsel ? 1 : 0;
            }
            if (ImGui::TableSetColumnIndex(1)) {
                std::string name_u = GbkToUtf8Str(r.name);
                int cur = -1;
                for (size_t k = 0; k < s_bare.size(); ++k)
                    if (AttrGInfoAliasU8(s_bare[k]) == name_u) { cur = (int)k; break; }
                std::vector<const char*> nc;
                std::string custom;
                int base = 0;
                if (cur < 0) {  // 未匹配: 原名置顶, 不显示为空
                    custom = name_u.empty() ? "（空）" : name_u;
                    nc.push_back(custom.c_str());
                    base = 1;
                    cur = 0;
                }
                for (auto& d : s_disp) nc.push_back(d.c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##nm", &cur, nc.data(), (int)nc.size())) {
                    int si = cur - base;
                    if (si >= 0 && si < (int)s_bare.size())
                        // 写 G_Info 名字(如 普通攻击力), 不是道具修改的名字(普通攻击)
                        r.name = Utf8ToGbkStr(AttrGInfoAliasU8(s_bare[(size_t)si]));
                }
            }
            if (ImGui::TableSetColumnIndex(2)) {
                int cc = 0;
                for (int x = 0; x < 5; ++x) if (color_letters[x] == r.color) { cc = x; break; }
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##col", &cc, color_items, 5)) r.color = color_letters[cc];
            }
            if (ImGui::TableSetColumnIndex(3)) {
                int pc = r.is_percent ? 1 : 0;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::Combo("##pct", &pc, pct_c.data(), (int)pct_c.size()))
                    r.is_percent = (pc == 1);
            }
            if (ImGui::TableSetColumnIndex(4)) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                int v = (int)r.vmin;
                if (ImGui::InputInt("##v", &v)) r.vmin = v;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("添加属性")) {
        poq::GamItem::GInfoRow r{};
        r.color = 0;
        auto lst = ItemData::AttrDisplayList();  // 默认取第一个属性名(写 G_Info 名字形式, GBK)
        r.name = lst.empty() ? std::string()
                             : Utf8ToGbkStr(AttrGInfoAliasU8(AttrBareNameU8(lst[0])));
        r.vmin = 0;
        r.has_max = false;
        r.is_percent = false;
        app.gam_grows.push_back(r);
        app.gam_grow_sel.push_back(0);
    }
    ImGui::SameLine();
    if (ImGui::Button("删除选中")) {   // 删除勾选的行
        for (int i = (int)app.gam_grows.size() - 1; i >= 0; --i) {
            if (i >= (int)app.gam_grow_sel.size() || !app.gam_grow_sel[(size_t)i]) continue;
            app.gam_grows.erase(app.gam_grows.begin() + i);
            app.gam_grow_sel.erase(app.gam_grow_sel.begin() + i);
        }
    }
    ImGui::NewLine();   // 提示另起一行
    ImGui::TextDisabled("改完后点\"保存文件\"即装配写回；模板头已隐藏、保存时自动补回。数值/上限可任意改（变长会自动重排）。");
}

static void RenderGamTab(App& app) {
    // 按钮行: 自动宽度 + 超宽自动换行(避免末尾按钮被挤出屏幕)
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    const float line_max_x = ImGui::GetContentRegionAvail().x;
    auto put_btn = [&](const char* text, auto fn) {
        float bw = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (ImGui::GetCursorPosX() + bw + 12.0f > line_max_x) ImGui::NewLine();
        if (ImGui::Button(text)) fn();
        ImGui::SameLine(0, 8.0f);
    };
    put_btn("打开gam", [&]{ GamOnOpen(app); });
    put_btn("保存文件", [&]{ GamOnSave(app); });
    put_btn("复制物品", [&]{ GamOnCopyItem(app); });
    put_btn("粘贴物品", [&]{ GamOnPasteItem(app); });
    put_btn("复制属性", [&]{ GamOnCopyAttr(app); });
    put_btn("粘贴属性", [&]{ GamOnPasteAttr(app); });
    ImGui::PopStyleVar();
    ImGui::NewLine();  // 结束按钮行, 状态回下一行
    // 状态(独立一行, 不挤按钮)
    if (app.gam_loaded)
        ImGui::TextDisabled("%s  |  物品 %d  容器 %d", app.gam_path.c_str(),
                            (int)app.gam_file.items.size(),
                            (int)app.gam_file.containers.size());

    // 创建新武器(目录): 类别定宽(固定), 具体物品占剩余宽度, 合上后悬停也显示属性
    ImGui::Separator();
    float row_x = ImGui::GetContentRegionAvail().x;
    const float lbl_w = ImGui::CalcTextSize("创建新武器：").x;
    const char* cat_preview = app.gam_cat_loaded ? app.gam_catalog.cats[(size_t)app.gam_sel_cat].c_str()
                                                 : "(目录未加载)";
    const float cat_w = 96.0f;  // 类别下拉固定宽度(不随内容/选中项变化)
    const float btn_w = ImGui::CalcTextSize("创建物品").x + ImGui::GetStyle().FramePadding.x * 2.0f + 10.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float item_w = row_x - lbl_w - cat_w - btn_w - spacing * 4;
    if (item_w < 120.0f) item_w = 120.0f;   // 窗口过窄时物品框至少 120px
    ImGui::TextUnformatted("创建新武器：");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(cat_w);
    if (ImGui::BeginCombo("##cat", cat_preview)) {
        if (!app.gam_cat_loaded) ImGui::TextDisabled("缺少 gam_catalog.bin");
        for (int i = 0; i < (int)app.gam_catalog.cats.size(); ++i)
            if (ImGui::Selectable(app.gam_catalog.cats[(size_t)i].c_str(), i == app.gam_sel_cat))
                app.gam_sel_cat = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const auto& ents = (app.gam_cat_loaded && app.gam_sel_cat < (int)app.gam_catalog.cats.size())
                           ? app.gam_catalog.items[(size_t)app.gam_sel_cat]
                           : std::vector<poq::GamCatalog::Entry>();
    static std::string preview_buf;
    if (!ents.empty() && app.gam_sel_cat_item < (int)ents.size()) {
        std::string dm = GamItemDispName(app, ents[(size_t)app.gam_sel_cat_item].name);
        preview_buf = "Lv" + std::to_string(ents[(size_t)app.gam_sel_cat_item].level) + " " + dm;
    } else {
        preview_buf = "选择物品";
    }
    ImGui::SetNextItemWidth(item_w);
    bool item_combo_open = ImGui::BeginCombo("##item", preview_buf.c_str(), ImGuiComboFlags_HeightLarge);
    static int tip_i = -1;
    static std::string tip;
    if (item_combo_open) {
        for (int i = 0; i < (int)ents.size(); ++i) {
            std::string lab = "Lv" + std::to_string(ents[(size_t)i].level) + " " +
                             GamItemDispName(app, ents[(size_t)i].name);
            lab += "##ci" + std::to_string(i);  // 重复物品名(如多个东海石)不冲突
            bool sel = (i == app.gam_sel_cat_item);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Selectable(lab.c_str(), sel)) app.gam_sel_cat_item = i;
            if (ImGui::IsItemHovered()) {
                if (tip_i != i) {
                    tip = GamFormatItemTip(app, ents[(size_t)i].rec);
                    tip_i = i;
                }
                if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        ImGui::EndCombo();
    } else {
        // 合上后悬停当前选中物品也显示属性
        if (ImGui::IsItemHovered() && !ents.empty() && app.gam_sel_cat_item >= 0 &&
            app.gam_sel_cat_item < (int)ents.size()) {
            if (tip_i != app.gam_sel_cat_item) {
                tip = GamFormatItemTip(app, ents[(size_t)app.gam_sel_cat_item].rec);
                tip_i = app.gam_sel_cat_item;
            }
            if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("创建物品")) GamOnCreate(app);
    // 提示独立一行, 避免把按钮挤出去
    ImGui::TextDisabled("（先选中左侧地图，再把目录物品新建到该地图；选中具体物品后可悬停看属性）");

    ImGui::Separator();

    if (!app.gam_loaded) {
        ImGui::TextDisabled("尚未打开 gam。点\"打开gam\"选择存档(默认定位到游戏目录)。");
        return;
    }

    // 左树 + 右编辑
    float avail = ImGui::GetContentRegionAvail().x;
    float w = avail * 0.42f;
    if (w < 260.0f) w = 260.0f;
    ImGui::BeginChild("gamleft", ImVec2(w, ImGui::GetContentRegionAvail().y),
                      ImGuiChildFlags_Borders);
    RenderGamTree(app);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("gamright", ImVec2(-FLT_MIN, ImGui::GetContentRegionAvail().y),
                      ImGuiChildFlags_Borders);
    RenderGamEdit(app);
    ImGui::EndChild();
}

// ============================================================================
// H2 加解密(task10): H2 数据文件原地加解密。
//   加密态: 首字节 0x80, 其余所有字节 ^0x78; 明文: GBK 编码, tab 分隔 csv。
//   解密: 首字节 0x80 则去掉后其余 ^0x78; 加密: 全部 ^0x78 后开头补 0x80。
//   转换前先备份原文件为 "<原名>.dec.bak"(解密) / "<原名>.enc.bak"(加密)。
// ============================================================================

std::wstring H2BackupPath(const std::wstring& path, bool /*decrypt*/) {
    // 命名与 gam 一致: <base>.<yyyymmddhhMMss>.bak.<ext>(加密/解密都备份)
    return TimestampedBakPath(path);
}

// 解密:首字节 0x80 则去掉后其余 ^0x78;否则视为已明文原样返回(python decrypt_bytes 同语义)。
std::vector<unsigned char> H2DecryptBytes(const std::vector<unsigned char>& d) {
    if (d.size() < 1 || d[0] != 0x80) return d;
    std::vector<unsigned char> out;
    out.reserve(d.size() - 1);
    for (size_t i = 1; i < d.size(); ++i)
        out.push_back(static_cast<unsigned char>(d[i] ^ 0x78));
    return out;
}
// 加密:全部 ^0x78 后开头补 0x80。
std::vector<unsigned char> H2EncryptBytes(const std::vector<unsigned char>& d) {
    std::vector<unsigned char> out;
    out.reserve(d.size() + 1);
    out.push_back(0x80);
    for (unsigned char c : d) out.push_back(static_cast<unsigned char>(c ^ 0x78));
    return out;
}

// 读取整个文件;失败返回 false。
bool H2ReadFile(const std::wstring& path, std::vector<unsigned char>& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

// 磁盘备份原文件(src -> dst,已存在则覆盖)。
bool H2CopyFile(const std::wstring& src, const std::wstring& dst) {
    return ::CopyFileW(src.c_str(), dst.c_str(), /*bFailIfExists=*/FALSE) != 0;
}

// 原地转换: 读原文件 -> 内存转换 -> 写同目录临时文件 -> MoveFileEx 原子替换原文件。
// 任意一步失败都保证原文件未更改(只读原文件,替换失败只删临时文件)。
bool H2ConvertFile(const std::wstring& path, bool decrypt) {
    std::vector<unsigned char> raw;
    if (!H2ReadFile(path, raw)) return false;
    std::vector<unsigned char> out = decrypt ? H2DecryptBytes(raw) : H2EncryptBytes(raw);
    std::wstring tmp = path + L".h2tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool wrote_ok = out.empty() || fwrite(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    if (!wrote_ok) {
        _wremove(tmp.c_str());
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        _wremove(tmp.c_str());
        return false;
    }
    return true;
}

// 执行转换(make_backup=true 时先备份原文件为 .dec/.enc.bak)。
void H2DoConvert(App& app, const std::wstring& path, bool decrypt, bool make_backup) {
    if (make_backup) {
        std::wstring bak = H2BackupPath(path, decrypt);
        if (!H2CopyFile(path, bak)) {
            MsgBox("转换失败", "备份文件创建失败，文件未更改，请检查文件", MB_ICONERROR);
            return;
        }
        TrackBackup(path, bak);   // 转换会改内容 -> 退出时 MD5 不同, 备份保留
    }
    if (!H2ConvertFile(path, decrypt)) {
        MsgBox("转换失败", "转换失败，文件未更改，请检查文件", MB_ICONERROR);
        return;
    }
    const char* ok = decrypt ? "解密成功，请用Excel编辑属性" : "加密成功，文件已恢复到乱码状态";
    MsgBox("转换成功", ok, MB_ICONINFORMATION);
    app.SetStatus(decrypt ? "H2 解密成功" : "H2 加密成功");
}

// 文件选择框初始目录:已连接/枚举到 poq.exe 时用其所在目录,否则工具所在目录。
std::wstring H2InitialDir(App& app) {
    std::optional<unsigned int> pid;
    if (app.modder.Mem().IsOpen())
        pid = app.modder.Pid();
    else if (auto s = TryGetSelectedPid(app))
        pid = s;
    if (!pid) pid = FindPoqPid(app);
    if (pid) {
        auto path = poq::ProcessMemory::FindModulePath(*pid, "POQ.exe");
        if (path) {
            std::string p = *path;
            size_t pos = p.find_last_of("/\\");
            if (pos != std::string::npos) {
                std::wstring dir = Utf8ToWide(p.substr(0, pos));
                if (!dir.empty() && ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
                    return dir;
            }
        }
    }
    return AppDirW();
}

// 选择文件后的统一入口:有备份 -> 弹三选一;否则直接转换。
void H2BeginConvert(App& app, const std::wstring& path, bool decrypt) {
    std::wstring bak = H2BackupPath(path, decrypt);
    if (::GetFileAttributesW(bak.c_str()) != INVALID_FILE_ATTRIBUTES) {
        app.h2_path = path;
        app.h2_decrypt = decrypt;
        app.h2_wait_confirm = true;
        return;
    }
    H2DoConvert(app, path, decrypt, /*make_backup=*/true);
}

// 点击解密/加密按钮:弹文件选择框,选好即转换。
void H2OnConvert(App& app, bool decrypt) {
    std::string sel = OpenFileDialog(H2InitialDir(app), L"所有文件 (*)\0*.*\0\0");
    if (sel.empty()) return;  // 用户取消
    H2BeginConvert(app, Utf8ToWide(sel), decrypt);
}

// 已有备份文件的三选一弹窗(覆盖备份转换 / 跳过备份转换 / 取消转换)。
void H2RenderBackupConfirm(App& app) {
    const char* popup_id = "备份已存在##h2bak";
    if (!ImGui::IsPopupOpen(popup_id)) ImGui::OpenPopup(popup_id);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(popup_id, nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        ImGui::PushTextWrapPos(win_w - 40.0f);
        ImGui::TextUnformatted("已存在备份文件，请选择：");
        ImGui::TextUnformatted(WideToUtf8(H2BackupPath(app.h2_path, app.h2_decrypt)).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14));
        const float btn_w = 150.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float total = btn_w * 3.0f + spacing * 2.0f;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);
        if (ImGui::Button("覆盖备份转换", ImVec2(btn_w, 0))) {
            H2DoConvert(app, app.h2_path, app.h2_decrypt, /*make_backup=*/true);
            app.h2_wait_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("跳过备份转换", ImVec2(btn_w, 0))) {
            H2DoConvert(app, app.h2_path, app.h2_decrypt, /*make_backup=*/false);
            app.h2_wait_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消转换", ImVec2(btn_w, 0))) {
            app.h2_wait_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// 目录选择回调:设置初始选中目录。
static int CALLBACK H2BrowseCallback(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) {
    if (uMsg == BFFM_INITIALIZED && lpData)
        ::SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, lpData);  // lpData = 初始目录宽串
    return 0;
}

// 目录选择框(SHBrowseForFolderW,自带 COM 处理,无需预初始化)。
std::wstring H2PickFolderDialog(const std::wstring& dir) {
    wchar_t disp[MAX_PATH]{};
    BROWSEINFOW bi{};
    bi.hwndOwner = nullptr;
    bi.pidlRoot = nullptr;
    bi.pszDisplayName = disp;
    bi.lpszTitle = L"选择要批量加解密的目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = H2BrowseCallback;
    bi.lParam = reinterpret_cast<LPARAM>(dir.c_str());
    PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return L"";  // 用户取消
    wchar_t path[MAX_PATH]{};
    bool ok = ::SHGetPathFromIDListW(pidl, path) != FALSE;
    ::CoTaskMemFree(pidl);
    if (!ok) return L"";
    std::wstring s(path);
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();  // 去末尾反斜杠(根目录除外)
    return s;
}

// 批量结果统计。
struct H2BatchResult {
    int ok = 0;       // 成功转换
    int skipped = 0;  // 跳过(方向已不符)
    int failed = 0;   // 失败(原文件未更改)
};

// 批量转换:递归目录(含子文件夹)内所有 .txt 文件。每个文件独立备份(已有备份则覆盖)。
// 解密只处理加密态(0x80 开头)文件,加密只处理明文文件,避免重复转换破坏数据。
H2BatchResult H2BatchConvert(const std::wstring& dir, bool decrypt) {
    H2BatchResult r;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& p = it->path();
        if (!it->is_regular_file(ec)) continue;
        if (_wcsicmp(p.extension().c_str(), L".txt") != 0) continue;  // 仅 txt 文件
        std::wstring wp = p.wstring();
        std::vector<unsigned char> raw;
        if (!H2ReadFile(wp, raw)) { r.failed++; continue; }
        bool is_enc = !raw.empty() && raw[0] == 0x80;
        if (decrypt ? !is_enc : is_enc) { r.skipped++; continue; }  // 方向不符,跳过
        std::wstring bak = H2BackupPath(wp, decrypt);
        if (!H2CopyFile(wp, bak)) { r.failed++; continue; }
        TrackBackup(wp, bak);
        if (!H2ConvertFile(wp, decrypt)) { r.failed++; continue; }
        r.ok++;
    }
    return r;
}

// 目录转换确认后执行,结果弹统计。
void H2RunBatch(App& app, const std::wstring& dir, bool decrypt) {
    H2BatchResult r = H2BatchConvert(dir, decrypt);
    std::string msg = (decrypt ? "解密" : "加密") + std::to_string(r.ok) + " 个文件";
    if (r.skipped > 0) msg += "，跳过 " + std::to_string(r.skipped) + " 个";
    if (r.failed > 0) msg += "，失败 " + std::to_string(r.failed) + " 个（原文件未更改）";
    if (r.failed > 0)
        MsgBox("目录转换", msg + "。", MB_ICONWARNING);
    else
        MsgBox("目录转换", msg + "。", MB_ICONINFORMATION);
    app.SetStatus((decrypt ? "H2 目录解密：" : "H2 目录加密：") + msg);
}

// 目录按钮点击:弹目录选择框,选好进确认弹窗。
void H2OnBatch(App& app, bool decrypt) {
    std::wstring dir = H2PickFolderDialog(H2InitialDir(app));
    if (dir.empty()) return;  // 用户取消
    app.h2_dir = dir;
    app.h2_dir_decrypt = decrypt;
    app.h2_wait_dir_confirm = true;
}

// 目录转换确认弹窗(提示会转换整个目录含子文件夹的 txt 文件,请做好备份)。
void H2RenderDirConfirm(App& app) {
    const char* popup_id = "目录转换确认##h2dir";
    if (!ImGui::IsPopupOpen(popup_id)) ImGui::OpenPopup(popup_id);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(popup_id, nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        ImGui::PushTextWrapPos(win_w - 40.0f);
        ImGui::TextUnformatted("将会转换整个目录（包括子文件夹）中的 txt 文件，请做好备份！");
        ImGui::TextUnformatted(
            app.h2_dir_decrypt ? "已是明文（解密态）的文件将自动跳过。"
                               : "已是加密态的文件将自动跳过。");
        ImGui::TextWrapped("目录：%s", WideToUtf8(app.h2_dir).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 14));
        const float btn_w = 140.0f;
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float total = btn_w * 2.0f + spacing;
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);
        if (ImGui::Button("开始转换", ImVec2(btn_w, 0))) {
            H2RunBatch(app, app.h2_dir, app.h2_dir_decrypt);
            app.h2_wait_dir_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(btn_w, 0))) {
            app.h2_wait_dir_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// H2解密 tab 主渲染。
void RenderH2Tab(App& app) {
    const float btn_w =
        ImGui::CalcTextSize("解密目录").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::TextDisabled("单个文件：");
    if (ImGui::Button("解密", ImVec2(btn_w, 0))) H2OnConvert(app, /*decrypt=*/true);
    ImGui::SameLine();
    if (ImGui::Button("加密", ImVec2(btn_w, 0))) H2OnConvert(app, /*decrypt=*/false);
    ImGui::TextDisabled("整个目录（含子文件夹）：");
    if (ImGui::Button("解密目录", ImVec2(btn_w, 0))) H2OnBatch(app, /*decrypt=*/true);
    ImGui::SameLine();
    if (ImGui::Button("加密目录", ImVec2(btn_w, 0))) H2OnBatch(app, /*decrypt=*/false);

    // 已有备份 -> 三选一弹窗
    if (app.h2_wait_confirm) H2RenderBackupConfirm(app);
    // 目录确认 -> 二选弹窗
    if (app.h2_wait_dir_confirm) H2RenderDirConfirm(app);
}

// ============================================================================
// 版本号:日期部分从编译期 __DATE__("Mmm dd yyyy")生成 YYMMDD,每次编译自动取当天;
// 后缀 "v2"(或 v3/v4...)为手动版本号,仅当用户说"用 v3/v4"才改这里。
// 启动弹窗显示形如 "Ver: 260913v1"。
// ============================================================================
constexpr int kVerMonth() {
    constexpr const char* names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int m = 1;
    for (int i = 0; i < 12; ++i) {
        bool eq = true;
        for (int j = 0; j < 3; ++j)
            if (__DATE__[j] != names[i][j]) { eq = false; break; }
        if (eq) { m = i + 1; break; }
    }
    return m;
}
constexpr int kVerYear2() { return (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0'); }
constexpr int kVerDay() {
    int d10 = (__DATE__[4] == ' ') ? 0 : (__DATE__[4] - '0');
    return d10 * 10 + (__DATE__[5] - '0');
}
const char* kVersionText() {
    static char buf[32];
    std::snprintf(buf, sizeof(buf), "Ver: %02d%02d%02dv2", kVerYear2(), kVerMonth(), kVerDay());
    return buf;
}

void RenderWelcomeDialog(App& app) {
    // 每次启动显示的免责声明弹窗:居中,底部中间"确认"关闭。
    if (!app.show_welcome) return;
    if (!ImGui::IsPopupOpen("关于")) ImGui::OpenPopup("关于");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 280), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("关于", nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        const float win_h = ImGui::GetWindowSize().y;
        const char* kLines[] = {
            "本软件仅供娱乐研究，严禁售卖牟利，一切后果与作者无关。",
            "唯一发布途径：百度贴吧",
            "作者：潜移默化的影响",
            "鸣谢：B站up口花生口",
        };
        // 文本区(四行)居中显示,垂直大致居中
        float y = win_h * 0.14f;
        const float line_h = ImGui::GetTextLineHeightWithSpacing();
        for (const char* line : kLines) {
            float tw = ImGui::CalcTextSize(line).x;
            ImGui::SetCursorPos(ImVec2((win_w - tw) * 0.5f, y));
            ImGui::TextUnformatted(line);
            y += line_h;
        }
        // 版本行:与上方内容空一行区分,同样居中(日期由编译期 __DATE__ 自动取当天)
        y += line_h;  // 空一行
        const char* ver = kVersionText();
        float tw = ImGui::CalcTextSize(ver).x;
        ImGui::SetCursorPos(ImVec2((win_w - tw) * 0.5f, y));
        ImGui::TextUnformatted(ver);
        // 底部中间"确认"按钮
        const float btn_w = 120.0f;
        const float btn_h = 30.0f;
        ImGui::SetCursorPos(ImVec2((win_w - btn_w) * 0.5f, win_h - btn_h - 20.0f));
        if (ImGui::Button("确认", ImVec2(btn_w, btn_h))) {
            app.show_welcome = false;
            ImGui::CloseCurrentPopup();
        }
        // Esc 也可关闭(且不再弹)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            app.show_welcome = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// 渲染待处理的消息弹窗(替代系统 MessageBox,样式与启动免责声明弹窗一致)。
// 多个消息排队:一次弹一个,点"确认"后显示下一个。
void RenderMsgQueue() {
    if (g_msgbox_queue.empty()) return;
    const MessageBoxMsg& m = g_msgbox_queue.front();
    // 标题栏文字 = 类型(提示/错误/警告),与"关于"弹窗标题栏对齐;
    // "##msgbox" 使窗口 ID 固定,不随类型变化。
    const char* kType = "提示";
    if (m.type == MB_ICONERROR) {
        kType = "错误";
    } else if (m.type == MB_ICONWARNING) {
        kType = "警告";
    }
    std::string popup_id = std::string(kType) + "##msgbox";
    if (!ImGui::IsPopupOpen(popup_id.c_str())) ImGui::OpenPopup(popup_id.c_str());
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);  // 固定宽,高随内容
    if (ImGui::BeginPopupModal(popup_id.c_str(), nullptr,
                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        // 内容(标题 + 逗号 + 正文,自动换行)
        std::string full = m.title + "，" + m.text;
        ImGui::PushTextWrapPos(win_w - 40.0f);
        ImGui::TextUnformatted(full.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 16));
        // 底部中间"确认"按钮
        if (m.on_answer) {
            // 三选一: 全部复制 / 只复制属性 / 取消复制
            const float bw = 96.0f;
            float total = bw * 3.0f + 24.0f;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - total) * 0.5f);
            const char* lb0 = m.b0.empty() ? "全部复制" : m.b0.c_str();
            const char* lb1 = m.b1.empty() ? "只复制属性" : m.b1.c_str();
            const char* lb2 = m.b2.empty() ? "取消复制" : m.b2.c_str();
            int pick = -1;
            if (ImGui::Button(lb0, ImVec2(bw, 30))) pick = 0;
            ImGui::SameLine(0, 12);
            if (ImGui::Button(lb1, ImVec2(bw, 30))) pick = 1;
            ImGui::SameLine(0, 12);
            if (ImGui::Button(lb2, ImVec2(bw, 30))) pick = 2;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) pick = 2;  // Esc = 取消
            if (pick >= 0) {
                auto fn = m.on_answer;
                g_msgbox_queue.erase(g_msgbox_queue.begin());
                ImGui::CloseCurrentPopup();
                if (fn) fn(pick);
            }
        } else {
            const float btn_w = 120.0f;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - btn_w) * 0.5f);
            if (ImGui::Button("确认", ImVec2(btn_w, 30)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                g_msgbox_queue.erase(g_msgbox_queue.begin());
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// 属性修改(角色)Tab:全局属性 + 角色选择 + 字段网格(task6 指定排列)。
void RenderCharTab(App& app) {
    // ---- 全局属性(队伍共享,置顶)----
    ImGui::TextUnformatted("全局属性");
    ImGui::Separator();
    ImGui::TextUnformatted("金钱:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("##money", app.money_buf, sizeof(app.money_buf));
    ImGui::SameLine();
    if (ImGui::Button("修改金钱")) OnMoneyApply(app);
    ImGui::SameLine();
    bool inv = app.ck_invincible;
    if (ImGui::Checkbox("无敌模式", &inv)) {
        app.ck_invincible = inv;
        OnInvincibleToggled(app, inv);
    }
    ImGui::SameLine();
    bool kill = app.ck_kill;
    if (ImGui::Checkbox("一击必杀", &kill)) {
        app.ck_kill = kill;
        OnKillToggled(app, kill);
    }

    // ---- 角色选择 + 读取/保存 ----
    ImGui::Separator();
    ImGui::TextUnformatted("角色:");
    ImGui::SameLine();
    std::vector<std::string> char_labels;
    for (size_t i = 0; i < app.char_names.size(); ++i) {
        // 下拉格式 = "角色N-职业-名字"(前缀"角色N-" + 前面角色类型,后面角色名字)
        auto it = KnownMembers().find(app.char_names[i]);
        std::string cls = (it != KnownMembers().end()) ? it->second + "-" : "";
        char_labels.push_back("角色" + std::to_string(i + 1) + "-" + cls +
                              app.char_names[i]);
    }
    std::vector<const char*> char_items;
    for (const auto& l : char_labels) char_items.push_back(l.c_str());
    int csel = app.char_selected;
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("##char", &csel, char_items.data(), static_cast<int>(char_items.size()))) {
        if (csel != app.char_selected && csel >= 0 &&
            static_cast<size_t>(csel) < app.char_names.size()) {
            app.char_selected = csel;
            RebuildCharWidgets(app);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("读取属性")) OnCharRead(app);
    ImGui::SameLine();
    if (ImGui::Button("保存属性")) OnCharSave(app);
    ImGui::SameLine();
    bool filter_on = app.char_filter;
    if (ImGui::Checkbox("角色属性校验过滤", &filter_on)) {
        if (!filter_on) app.char_filter_warn = true;
        app.char_filter = filter_on;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", app.char_status.c_str());
    if (!app.hidden && !app.char_slot_probe.empty()) {
        ImGui::TextDisabled("槽位探测:");
        for (size_t i = 0; i < app.char_slot_probe.size(); ++i) {
            ImGui::TextDisabled("  槽%zu=%s", i + 1, app.char_slot_probe[i].c_str());
        }
    }
    // 关闭"角色属性校验过滤"时的警告弹窗(居中、不可移动)
    if (app.char_filter_warn) {
        app.char_filter_warn = false;
        ImGui::OpenPopup("警告##charfilter");
    }
    ImGui::SetNextWindowSize(ImVec2(460, 170), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("警告##charfilter", nullptr,
                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoCollapse)) {
        // 每帧强制整窗居中(不依赖 Appearing 锚点,避免 AutoResize 导致偏位)
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetWindowPos(ImVec2(center.x - ImGui::GetWindowSize().x * 0.5f,
                                   center.y - ImGui::GetWindowSize().y * 0.5f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20);
        ImGui::TextWrapped("如果取消校验过滤，可能识别出其他角色，请谨慎修改！");
        ImGui::Spacing();
        const float btn_w = 120;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - btn_w) * 0.5f);
        ImGui::SetCursorPosY(ImGui::GetWindowSize().y - 40);
        if (ImGui::Button("确认", ImVec2(btn_w, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // ---- 字段网格(task6 指定排列:3 列行 + 2 列行,各行子项等宽)----
    static const char* kLayout[][3] = {
        {"姓名", "别名", "职业"},
        {"年龄", "性别", "等级"},
        {"经验", "下一级", "声望"},
        {"生命", "精气", nullptr},
        {"力量", "体质", nullptr},
        {"敏捷", "悟性", nullptr},
        {"魅力", "智慧", nullptr},
        {"普通攻击", "普通防御", nullptr},
        {"金攻击", "金防御", nullptr},
        {"木攻击", "木防御", nullptr},
        {"水攻击", "水防御", nullptr},
        {"火攻击", "火防御", nullptr},
        {"土攻击", "土防御", nullptr},
        {"闪避率", "命中率", nullptr},
        {"剩余属性点", "剩余技能点", nullptr},
    };

    // 渲染一个字段(标签在上,输入框在下,填满列宽;pair 两个输入框 + "/")。
    auto render_field = [&](const char* label) {
        int idx = FindCharField(label);
        if (idx < 0) {
            ImGui::TextDisabled("%s(未定义)", label);
            return;
        }
        const auto& f = CharFields()[static_cast<size_t>(idx)];
        CharWidget& w = app.char_widgets[static_cast<size_t>(idx)];
        bool disabled = app.char_addrs.empty() || IsCharFieldLocked(app, label);
        ImGui::TextUnformatted(label);
        ImGui::BeginDisabled(disabled);
        std::string id = "##ch" + std::to_string(idx);
        if (f.kind == CharKind::StringPtr) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText(id.c_str(), w.strbuf, sizeof(w.strbuf));
        } else if (f.kind == CharKind::Enum) {
            std::vector<std::pair<int, std::string>> opts;
            for (const auto& kv : f.enum_map)
                opts.emplace_back(kv.first, std::to_string(kv.first) + "：" + kv.second);
            std::vector<const char*> items;
            for (const auto& o : opts) items.push_back(o.second.c_str());
            int cur = -1;
            for (int j = 0; j < static_cast<int>(opts.size()); ++j)
                if (opts[static_cast<size_t>(j)].first == w.intval) {
                    cur = j;
                    break;
                }
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::Combo(id.c_str(), &cur, items.data(), static_cast<int>(items.size()))) {
                if (cur >= 0) w.intval = opts[static_cast<size_t>(cur)].first;
            }
        } else if (f.kind == CharKind::Pair) {
            float avail = ImGui::GetContentRegionAvail().x;
            float slash = ImGui::CalcTextSize("/").x + ImGui::GetStyle().ItemSpacing.x;
            float each = (avail - slash) * 0.5f;
            if (each < 30.0f) each = 30.0f;
            ImGui::SetNextItemWidth(each);
            ImGui::InputInt(("##chl" + std::to_string(idx)).c_str(), &w.pair_lo, 0, 0);
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted("/");
            ImGui::SameLine(0, 0);
            ImGui::SetNextItemWidth(each);
            ImGui::InputInt(("##chh" + std::to_string(idx)).c_str(), &w.pair_hi, 0, 0);
        } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt(id.c_str(), &w.intval, 0, 0);
        }
        ImGui::EndDisabled();
        if (f.hint) ImGui::TextDisabled("%s", f.hint->c_str());
    };

    float grid_h = ImGui::GetContentRegionAvail().y;
    if (grid_h < 80.0f) grid_h = 80.0f;
    ImGui::BeginChild("charfields", ImVec2(-FLT_MIN, grid_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    for (size_t r = 0; r < sizeof(kLayout) / sizeof(kLayout[0]); ++r) {
        int ncols = kLayout[r][2] ? 3 : 2;
        char tid[32];
        std::snprintf(tid, sizeof(tid), "charrow%zu", r);
        if (ImGui::BeginTable(tid, ncols)) {
            for (int c = 0; c < ncols; ++c)
                ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            for (int c = 0; c < ncols; ++c) {
                if (ImGui::TableSetColumnIndex(c)) render_field(kLayout[r][c]);
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void RenderTopBar(App& app) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    const float frame_pad_x = style.FramePadding.x * 2.0f;

    // 右侧按钮组总宽(刷新进程 + 初始化 + 帮助,用于让下拉框占满剩余宽度并右对齐)
    const char* kRefresh = "刷新进程";
    const char* kInit = "初始化";
    const char* kHelp = "帮助";
    const float refresh_w = ImGui::CalcTextSize(kRefresh).x + frame_pad_x;
    const float init_w = ImGui::CalcTextSize(kInit).x + frame_pad_x;
    const float help_w = ImGui::CalcTextSize(kHelp).x + frame_pad_x;
    const float buttons_w = refresh_w + init_w + help_w + spacing * 2.0f;

    // 游戏版本(mode)下拉项:显示 mode 名(v1.30 / Steam版)
    std::vector<std::string> ver_labels;
    for (const auto& kv : app.cfg.modes) ver_labels.push_back(kv.second.name);
    std::vector<const char*> ver_items;
    for (const auto& s : ver_labels) ver_items.push_back(s.c_str());

    // 进程下拉(POQ.EXE)
    std::vector<std::string> proc_labels;
    for (const auto& p : app.processes) proc_labels.push_back(p.Display());
    std::vector<const char*> proc_items;
    for (const auto& s : proc_labels) proc_items.push_back(s.c_str());

    // 版本标签固定 + 版本下拉固定宽;进程下拉占满剩余;按钮贴右缘
    const char* kVerLabel = "游戏版本:";
    const char* kProcLabel = "选择进程:";
    const float ver_label_w = ImGui::CalcTextSize(kVerLabel).x;
    const float proc_label_w = ImGui::CalcTextSize(kProcLabel).x;
    const float ver_w = 150.0f;
    const float row_w = ImGui::GetContentRegionAvail().x;
    float proc_w = row_w - ver_label_w - ver_w - proc_label_w - buttons_w - spacing * 4.0f;
    if (proc_w < 80.0f) proc_w = 80.0f;  // 窗口过窄时给进程下拉保底

    // 游戏版本切换 = 切换 offsets 模式:先还原旧模式注入,再切换并提示重新初始化
    ImGui::TextUnformatted(kVerLabel);
    ImGui::SameLine(0, spacing);
    ImGui::SetNextItemWidth(ver_w);
    int ver_sel = app.mode_selected;
    if (ImGui::Combo("##ver", &ver_sel, ver_items.data(), static_cast<int>(ver_items.size()))) {
        if (ver_sel != app.mode_selected && ver_sel >= 0 &&
            static_cast<size_t>(ver_sel) < app.mode_ids.size()) {
            const std::string& mid = app.mode_ids[static_cast<size_t>(ver_sel)];
            if (app.modder.Mem().IsOpen()) {
                try {
                    app.modder.Close();  // 还原旧模式注入,避免残留 hook
                } catch (...) {
                }
            }
            app.cfg.SetCurrent(mid);
            app.modder.Config() = app.cfg;
            app.ReloadCfgBuffers();
            app.RefreshProcesses();
            app.SetStatus("已切换游戏版本:" + app.cfg.Current().name + "。请重新初始化。");
        }
    }
    ImGui::SameLine(0, spacing);
    ImGui::TextUnformatted(kProcLabel);
    ImGui::SameLine(0, spacing);
    ImGui::SetNextItemWidth(proc_w);
    int sel = app.proc_selected;
    if (ImGui::Combo("##proc", &sel, proc_items.data(), static_cast<int>(proc_items.size()))) {
        app.proc_selected = sel;
    }
    ImGui::SameLine(0, spacing);
    if (ImGui::Button("刷新进程")) app.RefreshProcesses();
    ImGui::SameLine(0, spacing);
    if (ImGui::Button("初始化")) OnInit(app);
    ImGui::SameLine(0, spacing);
    if (ImGui::Button("帮助")) {
        // 弹窗打开瞬间跳到当前所在 tab 的说明(仅第一帧生效,之后可自由切换)
        app.help_tab = app.cur_tab;
        app.help_jump = true;
        ImGui::OpenPopup("帮助");
    }
}

// ============================================================================
// 使用说明弹窗:多 tab,tab 名与主界面一致;打开时自动跳到当前所在 tab。
// (内存查看/偏移设置为调试 tab,无使用说明)
// ============================================================================
struct HelpTabContent {
    const char* name;              // tab 名(与主界面一致)
    const char* const* steps;      // 步骤文本,每行一条
    int step_count;
};

const char* const kHelpCharSteps[] = {
    "1. 初始化:启动游戏并进入存档后,点顶部\"初始化\"打开游戏进程。",
    "2. 读取属性:在\"角色\"下拉框选择要修改的人物,点\"读取属性\",网格中显示该人物的属性。",
    "3. 修改:直接编辑网格中的各属性值;\"全局属性\"区为全队共享——金钱填好后点\"修改金钱\"写入,无敌模式/一击必杀勾选即生效。",
    "4. 保存属性:点\"保存属性\"把人物属性写回游戏内存。",
    "5. 存档:在游戏里存档,读档后可查看修改后的属性。",
};

const char* const kHelpItemSteps[] = {
    "1. 初始化:点\"初始化\"打开游戏进程(自动完成物品捕获,无需额外操作)。",
    "2. 钩住物品:在游戏里把要改的物品从物品栏中捡起来(注意必须从物品栏捡起,地上捡起无效),钩子会锁定它。",
    "3. 读取属性:点\"读取属性\",表格显示当前所有词条(同时自动缓存一份,供\"复制上一件物品\"使用);读不到或读到旧物品时,重复第 2 步再点读取。",
    "4. 编辑词条:直接修改数值、颜色、类型,或用\"添加/删除/清空\"调整数量;也可\"从模板读取\"、\"另存为\"批量处理。",
    "5. 保存属性:点\"保存属性\"写回内存。现有词条写回原链表节点;新增的词条由编辑器自动从节点池(编辑器管理的一块固定内存)分配并接到链尾,无需手工干预。",
    "6. 游戏内生效:保存后在游戏里把该物品扔到地上再捡起来,修改才会真正生效。",
    "7. 写入存档:确认生效后在游戏里存档,词条数值随物品一起写入存档文件,之后读档物品由游戏用原生内存重建,不再依赖编辑器。",
    "8. 复制上一件物品:改好一把并保存后,捡起新物品,不用点读取,直接点\"复制上一件物品\",上一件的词条会原样盖到新物品上(基础属性不复制),然后回到第 6 步生效。",
    "9. 重置节点池(日常不用点):每次读档后点一次最保险,回收全部槽位、零风险;看到\"节点池无空闲槽\"提示时,先确认改过的物品都已扔下捡回,再点重置,然后重新保存。注意:背包里还有带新增词条的物品时不要点,会废掉那些词条。",
};

const char* const kHelpSummonSteps[] = {
    "1. 版本差异:Steam 版支持较好,可召唤更多具体类别、攻击力普遍更强(刺客除外);v1.30 只有部分功能——不支持召唤朱厌、善翼、刺客和指定类型的秦兵,且召唤物攻击力弱、无法造成伤害、只能拉仇恨,但秦兵死亡有物品掉落。",
    "2. 选择目标:选择要召唤的类别和具体类型(阵营可选队友/盟友),点\"激活\"。",
    "3. 召唤:激活后,在游戏里让力士使用\"烽火狼烟\",即可召唤出对应怪物。",
    "4. 取消:激活状态下点\"取消\",恢复烽火狼烟默认值(召唤狼)。",
    "5. 读档后:需要重新点\"激活\",或点\"刷新召唤\"。",
    "6. 清理召唤物:先用 GM 命令 look 查到目标 ID。队友用 dismiss {ID} 解散(推荐,直接离队,无副作用);盟友不适用 dismiss,用 kill {ID} 逐个击杀(盟友太多可用 kill all 清除,但可能误杀 NPC 甚至破坏剧情,建议逐个清理)。操作前先存档并备份 System\\game\\player_{i}.gam。",
};

const char* const kHelpGmSteps[] = {
    "1. 激活:点\"激活\"按钮获取 GM 权限;一种方式无效时可尝试另一种,或按第 2 条手动激活(程序会静默建立 wizard.lst 文件并加入当前用户)。",
    "2. 手动激活方法:在\"游戏目录\\System\\wizard\\wizard.lst\"里加入\"9 {用户名}\"(单机用户名为 player,文件或目录不存在可自行创建),重启游戏,在游戏中按回车依次输入\"{$objectpoq$}\"和\"r 9\",屏幕出现激活提示。",
    "3. 执行命令:激活后在输入框输入 GM 命令,按回车或点\"执行\"按钮执行(工具内可以无视 GM 命令长度限制,但过长是否会崩溃没试过);可勾选\"执行次数\"批量执行。",
    "4. 历史命令:输入框内按上箭头向前翻最近执行过的命令(最多缓存 30 条,执行成功才入历史),下箭头往回翻;下翻翻过最新一条会还原浏览前正在输入的内容。焦点在常用命令列表时,上下箭头留给列表不切历史。",
    "5. 命令技巧:GM 命令可参考网上攻略,例如杀死杂货铺掌柜或铁匠铺掌柜获得装备,杀完后用创建命令恢复(名字可以用 look 看到)。",
    "6. 常用命令:放在工具目录的 gm_cmd.txt 中(每行\"命令#显示名\",# 后为显示名可省略),会出现在\"常用命令\"下拉里供快速输入,文件可自行编辑。",
};

const char* const kHelpH2Steps[] = {
    "1. 用途:游戏参数(如物品爆率)以加密形式存放在\"游戏目录\\Data\\\"下,本工具对这些文件原地解密/加密。",
    "2. 解密:选择 Data 目录下的加密文件点\"解密\";转换前自动备份原文件为 <名>.<yyyymmddhhMMss>.bak.<扩展名>。",
    "3. 编辑:解密后的文件是 GBK 编码的表格,请用 Excel 打开、编辑、保存;修改方法可参考论坛或其他教程。",
    "4. 加密:编辑完成后点\"加密\"转回加密格式,同样会先备份(加密/解密都备份);改错了可以用备份文件替换掉修改错误的文件恢复。",
    "5. 生效:修改后需要退出游戏重新进入才生效(存档+读档是否生效未确定)。",
};

const char* const kHelpGamSteps[] = {
    "1. 用途:直接编辑 .gam 存档文件里的物品(文件级,不依赖游戏内存/注入)。",
    "2. 打开:点\"打开 gam\"选择存档(游戏运行时默认进 System\\game 目录)。左侧树按\"地图→物品\"两级展开。打开时会自动备份原文件为 <名>.<yyyymmddhhMMss>.bak.gam。",
    "3. 选中:点物品名字选中它(右侧进入编辑);点地图/房间名选中该容器。上方\"记录长度 N 字节\"是该条记录的净内容大小。",
    "4. 词条表:一行一个属性。第一列可勾选(配\"删除选中\"批量删);\"添加属性\"加一行。属性名下拉框与\"道具修改\"同一张表;颜色/类型(数值·百分比)也在表里。上限是独立的一行——例如\"普通攻击\"配一行\"最大普通攻击\",保存时会自动合成回存档的 \"名 +值 ~ +上限\" 格式。",
    "5. 改值:内容变长或变短都由保存时自动重排(单/多地图存档均支持);若改短了,保存时会自动把记录补回原长度——因为内容变短会让游戏里地上的物品消失。",
    "6. 增删物品:物品行有\"删除\"(删除本件)、\"复制\"(复制本件记录);地图节点有\"删除\"(删除该地图下全部物品)、\"粘贴\"(把复制的物品粘贴到该地图);也可用\"创建物品\"从预置目录按类别→名字新建。",
    "7. 复制属性/粘贴属性:粘贴时会弹框让你选——名字/别名/类型 要不要一起粘贴。选\"只复制属性\"则这三项保持原样,其余属性照常粘贴;选\"取消复制\"则什么都不做。",
    "8. 保存:点\"保存文件\"写回(备份在打开时已自动生成,无需手动)。",
    "9. 非装备记录(技能等):词条里带 0x10x/0x11x 这类\"装备表之外\"id 的记录会标成[非装备记录],属性编辑只读——避免改写时丢掉它的词条。",
    "10. 备份清理:关闭窗口时会问一次——是否按 MD5 校验清理掉\"本次打开且没改动过\"的备份文件(校验要读文件,可能卡顿几秒;改过的备份一定会保留)。可点\"取消退出\"留在程序里。",
    "11. 注意:工具读的是文件。若刚在游戏里改过物品(打造/鉴定等),必须先在游戏里存档(F9),再在工具里重新\"打开 gam\"才能看到最新数据;在工具里改完存档文件后,进游戏读档生效。",
};

void RenderHelpPopup(App& app) {
    static const HelpTabContent kHelpTabs[] = {
        {"人物修改", kHelpCharSteps, 5},
        {"道具修改", kHelpItemSteps, 9},
        {"召唤修改", kHelpSummonSteps, 6},
        {"GM命令", kHelpGmSteps, 6},
        {"H2解密", kHelpH2Steps, 5},
        {"gam修改", kHelpGamSteps, 11},
    };
    // 固定尺寸(不随 tab 内容变化), 超出屏幕时收窄; 仍然居中、不可拖动、不可缩放
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float hw = 820.0f, hh = 620.0f;
    if (hw > vp.x - 80.0f) hw = vp.x - 80.0f;
    if (hh > vp.y - 80.0f) hh = vp.y - 80.0f;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(hw, hh), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("帮助", nullptr,
                                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoCollapse)) {
        return;
    }
    // 底部按钮行占的高度(正文区按它留白, 按钮固定在弹窗底部不随内容滚动)
    const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    if (ImGui::BeginTabBar("helptabs")) {
        const int kHelpTabCount = (int)(sizeof(kHelpTabs) / sizeof(kHelpTabs[0]));
        for (int i = 0; i < kHelpTabCount; ++i) {
            // 打开后的第一帧把当前主 tab 对应的说明设为选中,之后可自由切换。
            // help_tab 用主 tab 序号(0人物 1道具 2召唤 3GM 4H2 5gam); 调试 tab(6/7)没有说明页, 不跳。
            int want = (app.help_tab >= 0 && app.help_tab < kHelpTabCount) ? app.help_tab : -1;
            ImGuiTabItemFlags flags = (app.help_jump && i == want)
                                          ? ImGuiTabItemFlags_SetSelected
                                          : 0;
            if (ImGui::BeginTabItem(kHelpTabs[i].name, nullptr, flags)) {
                // 正文放进固定高度的子区域: 内容长了自动出纵向滚动条
                char body_id[64];
                std::snprintf(body_id, sizeof(body_id), "helpbody%d", i);
                ImGui::BeginChild(body_id, ImVec2(0, -footer_h), ImGuiChildFlags_None);
                ImGui::Spacing();
                ImGui::PushTextWrapPos(0.0f);  // 按正文区宽度换行
                for (int s = 0; s < kHelpTabs[i].step_count; ++s) {
                    ImGui::TextWrapped("%s", kHelpTabs[i].steps[s]);
                    ImGui::Spacing();
                }
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    app.help_jump = false;
    ImGui::Separator();
    if (ImGui::Button("关闭", ImVec2(ImGui::GetFontSize() * 6.0f, 0))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

// ============================================================================
// 灰白主题
// ============================================================================

void ApplyBlueTheme() {
    // 默认 imgui 蓝色主题(深色 + 蓝色强调色)
    ImGui::StyleColorsDark();
    // 标题栏统一蓝色(经典 Windows 蓝):"关于"/消息等弹窗一致。
    // 主窗口走系统标题栏(NoTitleBar),不受影响。
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.24f, 0.48f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.40f, 0.78f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.24f, 0.48f, 0.80f);
}

// ============================================================================
// 主窗口 / 主循环
// ============================================================================

void RenderMainWindow(App& app) {
    // 精确贴住系统窗口客户区(左上 (0,0) 开始,尺寸=工作区);
    // 去掉 imgui 自绘标题栏,用系统标题栏(标题一致),不重复。
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("秦殇修改器", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings);

    RenderTopBar(app);
    ImGui::Separator();

    // 选项卡区域(固定高度,把状态栏钉在底部)
    const float status_h = 26.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float tab_h = avail.y - status_h - 8.0f;
    if (tab_h < 100.0f) tab_h = 100.0f;

    ImGui::BeginChild("tabarea", ImVec2(avail.x, tab_h));
    app.gm_visible = false;  // 每帧先复位,GM tab 渲染时置 true(周期刷新只在可见时跑)
    if (ImGui::BeginTabBar("maintabs")) {
        if (ImGui::BeginTabItem("人物修改")) {
            app.cur_tab = 0;
            RenderCharTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("道具修改")) {
            app.cur_tab = 1;
            RenderBasicFields(app);
            ImGui::Separator();
            float table_h = ImGui::GetContentRegionAvail().y;
            if (table_h < 60.0f) table_h = 60.0f;
            RenderAttrTable(app, table_h);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("召唤修改")) {
            app.cur_tab = 2;
            RenderSummonTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("GM命令")) {
            app.cur_tab = 3;
            RenderGmTab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("H2解密")) {
            app.cur_tab = 4;
            RenderH2Tab(app);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("gam修改")) {
            app.cur_tab = 5;
            RenderGamTab(app);
            ImGui::EndTabItem();
        }
        if (!app.hidden && ImGui::BeginTabItem("内存查看")) {
            app.cur_tab = 6;
            RenderMemTab(app);
            ImGui::EndTabItem();
        }
        if (!app.hidden && ImGui::BeginTabItem("偏移设置")) {
            app.cur_tab = 7;
            RenderCfgTab(app);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    RenderHelpPopup(app);

    // 状态栏
    ImGui::Separator();
    ImGui::TextUnformatted(app.status.c_str());

    ImGui::End();
}

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
void CreateRenderTarget();
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
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
        &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)  // 无 GPU 驱动时回退 WARP 软件渲染
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2,
            D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel,
            &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
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

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
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
                g_exit_ask_pending = true;   // 交给主循环弹框
                return 0;                    // 先不关窗口
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
        "C:/Windows/Fonts/msyh.ttc",   // 微软雅黑
        "C:/Windows/Fonts/simhei.ttf",  // 黑体
        "C:/Windows/Fonts/simsun.ttc",  // 宋体
        "C:/Windows/Fonts/simsunb.ttf",
    };
    static const char* kBold[] = {
        "C:/Windows/Fonts/msyhbd.ttc",  // 微软雅黑 粗体
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsunb.ttf",
        "C:/Windows/Fonts/simsun.ttc",
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
    float main_scale =
        ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    // 创建窗口(图标来自 .rc 内嵌资源)
    HICON hIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
                      hIcon, nullptr, nullptr, nullptr, L"PoqEditorImgui", nullptr};
    wc.hIconSm = hIcon;
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"秦殇修改器",
                                WS_OVERLAPPEDWINDOW, 100, 100,
                                (int)(1100 * main_scale), (int)(720 * main_scale),
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        ::MessageBoxW(nullptr, L"DirectX 11 初始化失败(硬件/WARP 都不可用)。", L"PoqEditor", MB_ICONERROR);
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

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

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
        MsgBox("读取配置", "常用命令文件 gm_cmd.txt 有 " + std::to_string(gm_cmd_err) +
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

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) ==
                                       DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
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
        PollExitAsk();      // 收到关闭请求且本次生成过备份: 弹"是否清理备份"询问
        RenderMsgQueue();

        // 渲染
        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView,
                                                   (const float*)&clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // 关闭:还原自注入的字节并释放进程句柄
    try {
        app.modder.Close();
    } catch (...) {
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
