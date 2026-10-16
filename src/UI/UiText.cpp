// 秦殇物品修改器 (PoqEditor) —— 文本/工具/基础设施
// (#18 自 src/main.cpp 机械拆分, 代码逐字节搬移, 逻辑未改动。)。

#include <windows.h>
#include <commdlg.h>


#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <functional>
#include "UI/App.h"

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
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                    nullptr, nullptr);
    if (len <= 0) return "";
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], len, nullptr,
                          nullptr);
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
// 打开时创建的备份登记: 退出时判定"这个文件打开后有没有被改过", 没改过就删掉它的备份,
// 避免反复打开堆一堆没用的 .bak; 改过的备份保留(那种是有用的)。
// 判定分两级(为了快):
//   1) 先比"大小 + 修改时间"(打开备份时记下的) —— 没变就说明没动过, 直接删, 不读文件
//      (NTFS 的修改时间是 100ns 精度, 同尺寸改写也必然改动它)
//   2) 变了才把两边算 MD5 做最终判定(改过但内容恰好一样的情况, 比如游戏重存了同内容)
// ---------------------------------------------------------------------------
// 定义在 UI/App.h 里, 登记表本体在这里。
std::vector<OpenBackup> g_open_backups;

static bool StatFile(const std::wstring& path, uint64_t& size, uint64_t& mtime) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    mtime =
        ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

void TrackBackup(const std::wstring& src, const std::wstring& bak) {
    OpenBackup rec;
    rec.src = src;
    rec.bak = bak;
    StatFile(src, rec.size, rec.mtime);  // 此刻原文件还没被改
    g_open_backups.push_back(std::move(rec));
}
// ---- MD5(RFC 1321)精简实现(只用于上面的比对, 不依赖外部程序) ----
namespace {
struct Md5Ctx {
    uint32_t a = 0x67452301u, b = 0xefcdab89u, c = 0x98badcfeu, d = 0x10325476u;
    uint64_t len = 0;  // 已输入字节数
    unsigned char buf[64]{};
    size_t n = 0;  // buf 内已缓冲字节数
};
const uint32_t kMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u};
const int kMd5S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
inline uint32_t Md5Rotl(uint32_t x, int c) {
    return (x << c) | (x >> (32 - c));
}

void Md5Block(Md5Ctx& ctx, const unsigned char* p) {
    uint32_t m[16];
    std::memcpy(m, p, 64);  // x86 小端: 直接按 dword 解释, 省掉 64 次移位拼接
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
        if (::GetFileAttributesW(rec.bak.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;  // 备份已不在
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

std::vector<MessageBoxMsg> g_msgbox_queue;

void MsgBox(const std::string& title, const std::string& text, UINT type) {
    g_msgbox_queue.push_back(MessageBoxMsg{title, text, static_cast<int>(type), {}});
}

// 三选一询问框: 回调 0=全部复制 1=只复制属性(不动名字/别名/类型) 2=取消复制。
void MsgBoxAsk(const std::string& title, const std::string& text, std::function<void(int)> on) {
    g_msgbox_queue.push_back(
        MessageBoxMsg{title, text, static_cast<int>(MB_ICONQUESTION), std::move(on), {}, {}, {}});
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

// P5 收敛: 三处"名字/别名/类型要不要一起"询问框的公共管线(见 UiText.h)。
void AskIncludeIdent(App& app, const char* title, const std::string& body, const char* tail,
                     const char* cancel_msg,
                     const std::function<void(bool with_ident)>& on_choice) {
    std::string q = body + "\n\n" + tail;
    App* ap = &app;
    MsgBoxAsk(title, q, [ap, cancel_msg, on_choice](int choice) {
        if (choice == 2) {
            ap->SetStatus(cancel_msg);
            return;
        }
        on_choice(choice == 0);
    });
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
        std::snprintf(line, sizeof(line),
                      "%08llX: ", static_cast<unsigned long long>(base_addr + i));
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

}  // namespace poq
