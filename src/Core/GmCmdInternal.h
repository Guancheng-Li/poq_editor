#pragma once
// GmCmd 子模块共享声明(实现分散在 GmCmdUtil/Remote/capture.cpp).

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>
#include "Core/GmCmd.h"
#include "Native/ProcessMemory.h"

namespace poq::gm_cmd::detail {

constexpr uintptr_t kOffPlayerLev = 0x538;
extern const char kMarkExits[];
extern const char kMarkObjs[];
extern const char kObjIdPrefix[];

bool is_process_alive(unsigned int pid);
void append_le32(std::vector<unsigned char>& v, uint32_t x);

std::string trim(const std::string& s);
std::string to_lower(const std::string& s);
std::string gbk_to_utf8(const char* bytes, size_t n);
std::string utf8_to_gbk(const std::string& s);
std::string gbk_to_utf8_replace(const std::string& s);
std::wstring utf8_to_wide(const std::string& s);
std::string read_gbk_ptr(ProcessMemory& mem, uintptr_t a, int n);
std::vector<unsigned char> to_bytes(const std::string& s);
size_t find_bytes(const std::vector<unsigned char>& data, const std::string& kw, size_t off);
size_t r_find_nul(const std::vector<unsigned char>& data, size_t start, size_t end);
size_t find_nul_from(const std::vector<unsigned char>& data, size_t off);
std::vector<std::string> split_lines(const std::string& text);
std::vector<std::string> split_ws(const std::string& s);

const std::regex& re_scene();
const std::regex& re_exit();
const std::regex& re_obj_head();
const std::regex& re_obj();
const std::regex& re_obj_entry();
const std::regex& re_look_count();
const std::regex& re_look_obj();

uint32_t get_level(Ctx& ctx);
bool set_level(Ctx& ctx, uint32_t lv);
uint32_t read_rights(Ctx& ctx);

struct DispatchResult {
  bool timed_out = false;
  bool process_gone = false;
  bool failed = false;
  int64_t value = 0;
};
DispatchResult dispatch_direct(Ctx& ctx, const std::string& token, const std::string& rest,
                               int wait_ms, bool signextend);
DispatchResult send_command(Ctx& ctx, const std::string& token, const std::string& rest);

std::string read_first_alias(Ctx& ctx);
int fix_wizard_lst(Ctx& ctx, bool verbose, std::string& log);
void ensure_gm(Ctx& ctx);
void silent_prepare(Ctx& ctx);

bool look_stats_complete(const std::string& text);
std::string capture_obj_list(Ctx& ctx);
std::optional<std::string> capture130_text(Ctx& ctx, bool want_objs, double timeout_sec);
std::string capture_steam_text(Ctx& ctx, bool is_look);

}  // namespace poq::gm_cmd::detail
