// 秦殇物品修改器 (PoqEditor) —— H2 加解密 Tab(task10)
// (#18 自 src/main.cpp 机械拆分, 代码逐字节搬移, 逻辑未改动。)。

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>  // SHBrowseForFolderW(目录选择)
#include <shobjidl.h>


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

#include "UI/App.h"

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

namespace poq {

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
    for (size_t i = 1; i < d.size(); ++i) out.push_back(static_cast<unsigned char>(d[i] ^ 0x78));
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
        TrackBackup(path, bak);  // 转换会改内容 -> 退出时 MD5 不同, 备份保留
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
    if (ImGui::BeginPopupModal(
            popup_id, nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
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
    (void)lParam;  // 回调签名固定参数, 初始目录走 lpData
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
        if (!H2ReadFile(wp, raw)) {
            r.failed++;
            continue;
        }
        bool is_enc = !raw.empty() && raw[0] == 0x80;
        if (decrypt ? !is_enc : is_enc) {
            r.skipped++;
            continue;
        }  // 方向不符,跳过
        std::wstring bak = H2BackupPath(wp, decrypt);
        if (!H2CopyFile(wp, bak)) {
            r.failed++;
            continue;
        }
        TrackBackup(wp, bak);
        if (!H2ConvertFile(wp, decrypt)) {
            r.failed++;
            continue;
        }
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
    if (ImGui::BeginPopupModal(
            popup_id, nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        const float win_w = ImGui::GetWindowSize().x;
        ImGui::PushTextWrapPos(win_w - 40.0f);
        ImGui::TextUnformatted("将会转换整个目录（包括子文件夹）中的 txt 文件，请做好备份！");
        ImGui::TextUnformatted(app.h2_dir_decrypt ? "已是明文（解密态）的文件将自动跳过。"
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
    const float btn_w = ImGui::CalcTextSize("解密目录").x + ImGui::GetStyle().FramePadding.x * 2.0f;
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

}  // namespace poq
