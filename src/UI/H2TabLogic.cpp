// H2 Tab —— 加解密与批量转换逻辑。

#include "UI/H2TabLogic.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "Core/GameModder.h"
#include "Native/ProcessMemory.h"
#include "UI/App.h"

namespace poq {

std::wstring h2_backup_path(const std::wstring& path, bool /*decrypt*/) {
  // 命名与 gam 一致: <base>.<yyyymmddhhMMss>.bak.<ext>(加密/解密都备份)
  return timestamped_bak_path(path);
}

// 解密:首字节 0x80 则去掉后其余 ^0x78;否则视为已明文原样返回(python decrypt_bytes 同语义)。
std::vector<unsigned char> h2_decrypt_bytes(const std::vector<unsigned char>& d) {
  if (d.size() < 1 || d[0] != 0x80) return d;
  std::vector<unsigned char> out;
  out.reserve(d.size() - 1);
  for (size_t i = 1; i < d.size(); ++i) out.push_back(static_cast<unsigned char>(d[i] ^ 0x78));
  return out;
}
// 加密:全部 ^0x78 后开头补 0x80。
std::vector<unsigned char> h2_encrypt_bytes(const std::vector<unsigned char>& d) {
  std::vector<unsigned char> out;
  out.reserve(d.size() + 1);
  out.push_back(0x80);
  for (unsigned char c : d) out.push_back(static_cast<unsigned char>(c ^ 0x78));
  return out;
}

// 读取整个文件;失败返回 false。
bool h2_read_file(const std::wstring& path, std::vector<unsigned char>& out) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  unsigned char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return true;
}

// 磁盘备份原文件(src -> dst,已存在则覆盖)。
bool h2_copy_file(const std::wstring& src, const std::wstring& dst) {
  return ::CopyFileW(src.c_str(), dst.c_str(), /*bFailIfExists=*/FALSE) != 0;
}

// 原地转换: 读原文件 -> 内存转换 -> 写同目录临时文件 -> MoveFileEx 原子替换原文件。
// 任意一步失败都保证原文件未更改(只读原文件,替换失败只删临时文件)。
bool h2_convert_file(const std::wstring& path, bool decrypt) {
  std::vector<unsigned char> raw;
  if (!h2_read_file(path, raw)) return false;
  std::vector<unsigned char> out = decrypt ? h2_decrypt_bytes(raw) : h2_encrypt_bytes(raw);
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
void h2_do_convert(App& app, const std::wstring& path, bool decrypt, bool make_backup) {
  if (make_backup) {
    std::wstring bak = h2_backup_path(path, decrypt);
    if (!h2_copy_file(path, bak)) {
      msg_box("转换失败", "备份文件创建失败，文件未更改，请检查文件", MB_ICONERROR);
      return;
    }
    track_backup(path, bak);  // 转换会改内容 -> 退出时 MD5 不同, 备份保留
  }
  if (!h2_convert_file(path, decrypt)) {
    msg_box("转换失败", "转换失败，文件未更改，请检查文件", MB_ICONERROR);
    return;
  }
  const char* ok = decrypt ? "解密成功，请用Excel编辑属性" : "加密成功，文件已恢复到乱码状态";
  msg_box("转换成功", ok, MB_ICONINFORMATION);
  app.set_status(decrypt ? "H2 解密成功" : "H2 加密成功");
}

// 文件选择框初始目录:已连接/枚举到 poq.exe 时用其所在目录,否则工具所在目录。
std::wstring h2_initial_dir(App& app) {
  std::optional<unsigned int> pid;
  if (app.modder().mem().is_open())
    pid = app.modder().pid();
  else if (auto s = try_get_selected_pid(app))
    pid = s;
  if (!pid) pid = find_poq_pid(app);
  if (pid) {
    auto path = poq::ProcessMemory::find_module_path(*pid, "POQ.exe");
    if (path) {
      std::string p = *path;
      size_t pos = p.find_last_of("/\\");
      if (pos != std::string::npos) {
        std::wstring dir = utf8_to_wide(p.substr(0, pos));
        if (!dir.empty() && ::GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES)
          return dir;
      }
    }
  }
  return app_dir_w();
}

// 选择文件后的统一入口:有备份 -> 弹三选一;否则直接转换。
void h2_begin_convert(App& app, const std::wstring& path, bool decrypt) {
  std::wstring bak = h2_backup_path(path, decrypt);
  if (::GetFileAttributesW(bak.c_str()) != INVALID_FILE_ATTRIBUTES) {
    app.h2_path = path;
    app.h2_decrypt = decrypt;
    app.h2_wait_confirm = true;
    return;
  }
  h2_do_convert(app, path, decrypt, /*make_backup=*/true);
}

// 点击解密/加密按钮:弹文件选择框,选好即转换。
void h2_on_convert(App& app, bool decrypt) {
  std::string sel = open_file_dialog(h2_initial_dir(app), L"所有文件 (*)\0*.*\0\0");
  if (sel.empty()) return;  // 用户取消
  h2_begin_convert(app, utf8_to_wide(sel), decrypt);
}

// 目录选择回调:设置初始选中目录。
static int CALLBACK h2_browse_callback(HWND hwnd, UINT uMsg, LPARAM lParam, LPARAM lpData) {
  (void)lParam;  // 回调签名固定参数, 初始目录走 lpData
  if (uMsg == BFFM_INITIALIZED && lpData)
    ::SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, lpData);  // lpData = 初始目录宽串
  return 0;
}

// 目录选择框(SHBrowseForFolderW,自带 COM 处理,无需预初始化)。
std::wstring h2_pick_folder_dialog(const std::wstring& dir) {
  wchar_t disp[MAX_PATH]{};
  BROWSEINFOW bi{};
  bi.hwndOwner = nullptr;
  bi.pidlRoot = nullptr;
  bi.pszDisplayName = disp;
  bi.lpszTitle = L"选择要批量加解密的目录";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  bi.lpfn = h2_browse_callback;
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
H2BatchResult h2_batch_convert(const std::wstring& dir, bool decrypt) {
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
    if (!h2_read_file(wp, raw)) {
      r.failed++;
      continue;
    }
    bool is_enc = !raw.empty() && raw[0] == 0x80;
    if (decrypt ? !is_enc : is_enc) {
      r.skipped++;
      continue;
    }  // 方向不符,跳过
    std::wstring bak = h2_backup_path(wp, decrypt);
    if (!h2_copy_file(wp, bak)) {
      r.failed++;
      continue;
    }
    track_backup(wp, bak);
    if (!h2_convert_file(wp, decrypt)) {
      r.failed++;
      continue;
    }
    r.ok++;
  }
  return r;
}

// 目录转换确认后执行,结果弹统计。
void h2_run_batch(App& app, const std::wstring& dir, bool decrypt) {
  H2BatchResult r = h2_batch_convert(dir, decrypt);
  std::string msg = (decrypt ? "解密" : "加密") + std::to_string(r.ok) + " 个文件";
  if (r.skipped > 0) msg += "，跳过 " + std::to_string(r.skipped) + " 个";
  if (r.failed > 0) msg += "，失败 " + std::to_string(r.failed) + " 个（原文件未更改）";
  if (r.failed > 0)
    msg_box("目录转换", msg + "。", MB_ICONWARNING);
  else
    msg_box("目录转换", msg + "。", MB_ICONINFORMATION);
  app.set_status((decrypt ? "H2 目录解密：" : "H2 目录加密：") + msg);
}

// 目录按钮点击:弹目录选择框,选好进确认弹窗。
void h2_on_batch(App& app, bool decrypt) {
  std::wstring dir = h2_pick_folder_dialog(h2_initial_dir(app));
  if (dir.empty()) return;  // 用户取消
  app.h2_dir = dir;
  app.h2_dir_decrypt = decrypt;
  app.h2_wait_dir_confirm = true;
}
}  // namespace poq
