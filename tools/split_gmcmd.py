from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "src" / "Core"
lines = (ROOT / "GmCmd.cpp").read_text(encoding="utf-8").splitlines(keepends=True)


def sl(a, b):
    return "".join(lines[a - 1 : b])


detail_prefix = (
    '#include "Core/GmCmdInternal.h"\n\n'
    "#include <windows.h>\n#include <algorithm>\n#include <cctype>\n#include <chrono>\n"
    "#include <cmath>\n#include <cstdio>\n#include <cstdlib>\n#include <cstring>\n"
    "#include <filesystem>\n#include <map>\n#include <optional>\n#include <regex>\n"
    "#include <set>\n#include <stdexcept>\n#include <utility>\n\n"
    "namespace poq::gm_cmd::detail {\n\n"
)
detail_suffix = "}  // namespace poq::gm_cmd::detail\n"

(ROOT / "GmCmdUtil.cpp").write_text(detail_prefix + sl(36, 258) + detail_suffix, encoding="utf-8")
(ROOT / "GmCmdRemote.cpp").write_text(detail_prefix + sl(260, 571) + detail_suffix, encoding="utf-8")
(ROOT / "GmCmdCapture.cpp").write_text(detail_prefix + sl(573, 799) + detail_suffix, encoding="utf-8")

public_src = (
    '#include "Core/GmCmd.h"\n#include "Core/GmCmdInternal.h"\n\n'
    "namespace poq::gm_cmd {\n\n"
    "using detail::DispatchDirect;\nusing detail::SendCommand;\nusing detail::SilentPrepare;\n"
    "using detail::GetLevel;\nusing detail::SetLevel;\nusing detail::ReadRights;\n"
    "using detail::FixWizardLst;\nusing detail::CaptureObjList;\nusing detail::Capture130Text;\n"
    "using detail::CaptureSteamText;\n\n"
    + sl(806, 905)
    + "}  // namespace poq\n"
)
(ROOT / "GmCmd.cpp").write_text(public_src, encoding="utf-8")

internal_h = '''#pragma once
// GmCmd 子模块共享声明(实现分散在 GmCmdUtil/Remote/Capture.cpp).

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "Core/GmCmd.h"
#include "Native/ProcessMemory.h"

namespace poq::gm_cmd::detail {

constexpr uintptr_t kOffPlayerLev = 0x538;

std::string Trim(const std::string& s);
std::string ToLower(const std::string& s);
std::string GbkToUtf8(const char* bytes, size_t n);
std::string Utf8ToGbk(const std::string& s);
std::string GbkToUtf8Replace(const std::string& s);
std::wstring Utf8ToWide(const std::string& s);
std::string ReadGbkPtr(ProcessMemory& mem, uintptr_t a, int n);
std::vector<unsigned char> ToBytes(const std::string& s);
size_t FindBytes(const std::vector<unsigned char>& data, const std::string& kw, size_t off);
size_t RFindNul(const std::vector<unsigned char>& data, size_t start, size_t end);
size_t FindNulFrom(const std::vector<unsigned char>& data, size_t off);
std::vector<std::string> SplitLines(const std::string& text);
std::vector<std::string> SplitWs(const std::string& s);

const std::regex& ReScene();
const std::regex& ReExit();
const std::regex& ReObjHead();
const std::regex& ReObj();
const std::regex& ReObjEntry();
const std::regex& ReLookCount();
const std::regex& ReLookObj();

uint32_t GetLevel(Ctx& ctx);
bool SetLevel(Ctx& ctx, uint32_t lv);
uint32_t ReadRights(Ctx& ctx);

struct DispatchResult {
    bool timed_out = false;
    bool process_gone = false;
    bool failed = false;
    int64_t value = 0;
};
DispatchResult DispatchDirect(Ctx& ctx, const std::string& token, const std::string& rest,
                              int wait_ms, bool signextend);
DispatchResult SendCommand(Ctx& ctx, const std::string& token, const std::string& rest);

std::string ReadFirstAlias(Ctx& ctx);
int FixWizardLst(Ctx& ctx, bool verbose, std::string& log);
void EnsureGm(Ctx& ctx);
void SilentPrepare(Ctx& ctx);

bool LookStatsComplete(const std::string& text);
std::string CaptureObjList(Ctx& ctx);
std::optional<std::string> Capture130Text(Ctx& ctx, bool want_objs, double timeout_sec);
std::string CaptureSteamText(Ctx& ctx, bool is_look);

}  // namespace poq::gm_cmd::detail
'''
(ROOT / "GmCmdInternal.h").write_text(internal_h, encoding="utf-8")
print("GmCmd split written")
