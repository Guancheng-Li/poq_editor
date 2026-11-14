# One-off splitter for GameModder.cpp / GmCmd.cpp (run from repo root).
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "src" / "Core"


def split_gamemodder():
    lines = (ROOT / "GameModder.cpp").read_text(encoding="utf-8").splitlines(keepends=True)

    def sl(a, b):
        return "".join(lines[a - 1 : b])

    inner = sl(14, 103)
    internal_h = (
        "#pragma once\n"
        "// GameModder 子模块共享小工具(注入/读写/词条池).\n\n"
        "#include <cstdint>\n#include <cstdio>\n#include <string>\n#include <vector>\n\n"
        "namespace poq::modder_detail {\n"
        + inner
        + "}  // namespace poq::modder_detail\n"
    )
    (ROOT / "GameModderInternal.h").write_text(internal_h, encoding="utf-8")

    common = (
        '#include "Core/GameModder.h"\n'
        '#include "Core/GameModderInternal.h"\n\n'
        "#include <algorithm>\n#include <cstdint>\n#include <cstdio>\n"
        "#include <cstring>\n#include <stdexcept>\n\n#include <windows.h>\n\n"
        "namespace poq {\n\n"
        "using modder_detail::AppendCmpEsiImm;\n"
        "using modder_detail::AppendLe32;\n"
        "using modder_detail::AddrHex8;\n"
        "using modder_detail::HexBytes;\n"
        "using modder_detail::GbkEncodeOne;\n"
        "using modder_detail::Utf8ToCodepoints;\n\n"
    )
    (ROOT / "GameModderInject.cpp").write_text(common + sl(106, 642) + "}  // namespace poq\n", encoding="utf-8")
    (ROOT / "GameModderFields.cpp").write_text(common + sl(648, 871) + "}  // namespace poq\n", encoding="utf-8")
    (ROOT / "GameModderAttrs.cpp").write_text(common + sl(877, 1110) + "}  // namespace poq\n", encoding="utf-8")
    (ROOT / "GameModder.cpp").write_text(
        '#include "Core/GameModder.h"\n\nnamespace poq {\n\n' + sl(1116, 1118) + "}  // namespace poq\n",
        encoding="utf-8",
    )


def split_gmcmd():
    lines = (ROOT / "GmCmd.cpp").read_text(encoding="utf-8").splitlines(keepends=True)

    def sl(a, b):
        return "".join(lines[a - 1 : b])

    # detail block: line 35-801 (inside anonymous namespace, before public API)
    detail_inner = sl(36, 801)
    internal_h = (
        "#pragma once\n"
        "// GmCmd 子模块共享(编码/远程 dispatch/捕获/wizard).\n\n"
        "#include <cstdint>\n#include <regex>\n#include <string>\n#include <vector>\n\n"
        '#include "Core/GmCmd.h"\n#include "Native/ProcessMemory.h"\n\n'
        "namespace poq::gm_cmd::detail {\n"
        + detail_inner
        + "}  // namespace poq::gm_cmd::detail\n"
    )
    (ROOT / "GmCmdInternal.h").write_text(internal_h, encoding="utf-8")

    remote = (
        '#include "Core/GmCmd.h"\n#include "Core/GmCmdInternal.h"\n\n'
        "#include <stdexcept>\n\n"
        "namespace poq::gm_cmd {\n\n"
        "using detail::DispatchDirect;\nusing detail::SendCommand;\n"
        "using detail::SilentPrepare;\nusing detail::EnsureGm;\n"
        "using detail::GetLevel;\nusing detail::SetLevel;\nusing detail::ReadRights;\n"
        "using detail::FixWizardLst;\nusing detail::ReadFirstAlias;\n\n"
        + sl(806, 822)
        + sl(818, 822)  # duplicate - fix manually
    )
    # Rebuild GmCmd files properly with explicit sections
    util_body = sl(36, 258)  # through SplitWs
    rights_body = sl(260, 296)
    remote_body = sl(298, 407)  # Dispatch + SendCommand
    wizard_body = sl(409, 544)
    prepare_body = sl(546, 571)
    capture_body = sl(573, 799)  # through CaptureSteamText
    public_body = sl(803, 905)

    wrap_detail = (
        '#include "Core/GmCmd.h"\n#include "Core/GmCmdInternal.h"\n\n'
        "namespace poq::gm_cmd::detail {\n\n"
    )
    end_detail = "}  // namespace poq::gm_cmd::detail\n"

    (ROOT / "GmCmdUtil.cpp").write_text(wrap_detail + util_body + end_detail, encoding="utf-8")
    (ROOT / "GmCmdRemote.cpp").write_text(
        wrap_detail + rights_body + "\n" + remote_body + "\n" + prepare_body + "\n" + wizard_body + end_detail,
        encoding="utf-8",
    )
    (ROOT / "GmCmdCapture.cpp").write_text(wrap_detail + capture_body + end_detail, encoding="utf-8")
    (ROOT / "GmCmd.cpp").write_text(
        '#include "Core/GmCmd.h"\n#include "Core/GmCmdInternal.h"\n\nnamespace poq::gm_cmd {\n\n'
        + public_body
        + "}  // namespace poq\n",
        encoding="utf-8",
    )

    # Fix internal.h to only declare - actually we put implementations in detail cpp files
    # Internal.h should be declarations only for cross-file - simpler: put ALL detail impl in one GmCmdDetail.cpp
    # User asked split by module - Remote cpp includes wizard in same file is OK

    # Regenerate internal.h as include aggregator + forward decls only
    (ROOT / "GmCmdInternal.h").write_text(
        "#pragma once\n"
        '#include "Core/GmCmd.h"\n'
        "#include <cstdint>\n#include <regex>\n#include <string>\n#include <vector>\n\n"
        "namespace poq::gm_cmd::detail {\n"
        "// 实现在 GmCmdUtil / GmCmdRemote / GmCmdCapture .cpp\n"
        "std::string Trim(const std::string& s);\n"
        "std::string GbkToUtf8(const char* bytes, size_t n);\n"
        "std::string Utf8ToGbk(const std::string& s);\n"
        "struct DispatchResult {\n"
        "    bool timed_out = false;\n"
        "    bool process_gone = false;\n"
        "    bool failed = false;\n"
        "    int64_t value = 0;\n"
        "};\n"
        "DispatchResult DispatchDirect(poq::gm_cmd::Ctx& ctx, const std::string& token,\n"
        "                              const std::string& rest, int wait_ms, bool signextend);\n"
        "DispatchResult SendCommand(poq::gm_cmd::Ctx& ctx, const std::string& token, const std::string& rest);\n"
        "void SilentPrepare(poq::gm_cmd::Ctx& ctx);\n"
        "uint32_t GetLevel(poq::gm_cmd::Ctx& ctx);\n"
        "bool SetLevel(poq::gm_cmd::Ctx& ctx, uint32_t lv);\n"
        "uint32_t ReadRights(poq::gm_cmd::Ctx& ctx);\n"
        "int FixWizardLst(poq::gm_cmd::Ctx& ctx, bool verbose, std::string& log);\n"
        "std::string CaptureObjList(poq::gm_cmd::Ctx& ctx);\n"
        "std::optional<std::string> Capture130Text(poq::gm_cmd::Ctx& ctx, bool want_objs, double timeout_sec);\n"
        "std::string CaptureSteamText(poq::gm_cmd::Ctx& ctx, bool is_look);\n"
        "const std::regex& ReLookCount();\n"
        "const std::regex& ReLookObj();\n"
        "}  // namespace poq::gm_cmd::detail\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    split_gamemodder()
    # Restore GmCmd from git first if already broken
    print("done gamemodder")
