// GameModder —— 捕获 cave / 冻结(从 GameModderInject 拆出)。
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>
#include "Core/GameModder.h"
#include "Core/GameModderInternal.h"

namespace poq {

using modder_detail::append_cmp_esi_imm;
using modder_detail::append_le32;

void GameModder::set_capture_frozen(bool on) {
  if (!freeze_addr_) return;
  try {
    mem_.write_u_int32(freeze_addr_, on ? 1u : 0u);
  } catch (...) {
  }
}

std::map<std::string, GameModder::CaptureBuilder> GameModder::capture_builders() {
  // 捕获条件注册表:新增条件类型只需在此注册 + 配置 capture.type,无 if-else。
  static const std::map<std::string, CaptureBuilder> kBuilders = {
      {"weight", &GameModder::build_cap_weight},
      {"item_type", &GameModder::build_cap_item_type},
  };
  return kBuilders;
}

std::vector<unsigned char> GameModder::build_cap_weight(uintptr_t cur_item_addr,
                                                        const CaptureCfg& cap) {
  // 捕获条件 weight(v1.30 默认):重量条件,占用的寄存器入栈保护。
  //   push ecx
  //   mov ecx,[esi+offset]      ; 重量
  //   cmp ecx, max
  //   ja nocap
  //   mov [cur_item], esi       ; 捕获物品指针
  // nocap:
  //   pop ecx
  std::vector<unsigned char> cave;
  cave.push_back(0x51);  // push ecx
  cave.push_back(0x8B);
  cave.push_back(0x8E);  // mov ecx,[esi+offset]
  append_le32(cave, cap.offset);
  cave.push_back(0x81);
  cave.push_back(0xF9);  // cmp ecx, max
  append_le32(cave, cap.max);
  cave.push_back(0x77);
  cave.push_back(0x06);  // ja +6(跳过 mov [cur_item],esi)
  cave.push_back(0x89);
  cave.push_back(0x35);  // mov [cur_item],esi
  append_le32(cave, static_cast<uint32_t>(cur_item_addr));
  cave.push_back(0x59);  // pop ecx (nocap)
  return cave;
}

std::vector<unsigned char> GameModder::build_cap_item_type(uintptr_t cur_item_addr,
                                                           const CaptureCfg& cap) {
  // 捕获条件 item_type(steam/web.ct):类型条件,不占寄存器。
  //   cmp [esi+mark_offset], mark_value
  //   jne code
  //   cmp [esi+type_offset], type_min
  //   jb code
  //   cmp [esi+type_offset], type_max
  //   ja code
  //   mov [cur_item], esi
  // code:
  std::vector<unsigned char> conds;
  append_cmp_esi_imm(conds, cap.mark_offset, static_cast<int32_t>(cap.mark_value));
  size_t j1 = conds.size();
  conds.push_back(0x75);
  conds.push_back(0x00);  // jne code
  append_cmp_esi_imm(conds, cap.type_offset, static_cast<int32_t>(cap.type_min));
  size_t j2 = conds.size();
  conds.push_back(0x72);
  conds.push_back(0x00);  // jb code
  append_cmp_esi_imm(conds, cap.type_offset, static_cast<int32_t>(cap.type_max));
  size_t j3 = conds.size();
  conds.push_back(0x77);
  conds.push_back(0x00);  // ja code
  conds.push_back(0x89);
  conds.push_back(0x35);  // mov [cur_item],esi
  append_le32(conds, static_cast<uint32_t>(cur_item_addr));
  size_t code_pos = conds.size();
  // 回填三条条件跳转的目标(code 标签)
  conds[j1 + 1] = static_cast<unsigned char>((code_pos - (j1 + 2)) & 0xFF);
  conds[j2 + 1] = static_cast<unsigned char>((code_pos - (j2 + 2)) & 0xFF);
  conds[j3 + 1] = static_cast<unsigned char>((code_pos - (j3 + 2)) & 0xFF);
  return conds;
}

std::vector<unsigned char> GameModder::build_cave(uintptr_t cur_item_addr, uintptr_t cave_addr,
                                                  uintptr_t return_addr, uint8_t disp,
                                                  const CaptureCfg& capture, uintptr_t freeze_addr,
                                                  uintptr_t bag_flag_addr, uint32_t bag_flag_value,
                                                  uintptr_t flag_last_addr,
                                                  uintptr_t flag_arm_addr) {
  // "判断 + 捕获"段由捕获条件注册表按 capture.type 生成(无 if-else);
  // 这里只负责拼接原指令段(与 CT 等价)和 jmp 回跳。
  auto it = capture_builders().find(capture.type);
  if (it == capture_builders().end())
    throw std::runtime_error("未知捕获条件类型: " + capture.type + "(配置 hook.capture.type)");
  std::vector<unsigned char> capture_code = it->second(cur_item_addr, capture);
  size_t capt_end = capture_code.size();  // 捕获段的 "code:" 标签位置
  std::vector<unsigned char> cave;
  // 跳转占位:(rel 字节位置, kind) kind: 0=.noedge  1=.skip
  std::vector<std::pair<size_t, int>> jmp8;
  std::vector<std::pair<size_t, int>> jmp32;
  size_t noedge_label = 0;  // .noedge 标签位置(边缘判定后、"cmp [arm],0" 处)
  auto emit8 = [&](int kind) {
    jmp8.emplace_back(cave.size(), kind);
    cave.push_back(0x00);
  };
  auto emit32 = [&](int kind) {
    jmp32.emplace_back(cave.size(), kind);
    cave.push_back(0x00);
    cave.push_back(0x00);
    cave.push_back(0x00);
    cave.push_back(0x00);
  };
  if (freeze_addr) {
    // 冻结门: freeze!=0 跳过捕获段。cmp dword [freeze],0 ; jne skip
    cave.push_back(0x83);
    cave.push_back(0x3D);
    append_le32(cave, static_cast<uint32_t>(freeze_addr));
    cave.push_back(0x00);
    cave.push_back(0x75);
    emit8(1);
  }
  if (bag_flag_addr) {
    // 背包拿起臂窗门(容忍 flag 先/后于读属性):
    //   0->1 边缘 -> 记 last、设 arm=1、jmp skip(本拍不捕);下一拍 arm==1 -> arm=0 捕获一次。
    //   悬停/场景期 flag 平直 -> 无边缘且 arm 早已 0 -> 一律跳过,不入闩锁。
    // ecx=临时寄存器(原指令只重写 eax/esi,不依赖 ecx)。
    cave.push_back(0x8B);
    cave.push_back(0x0D);  // mov ecx,[flag]
    append_le32(cave, static_cast<uint32_t>(bag_flag_addr));
    cave.push_back(0x3B);
    cave.push_back(0x0D);  // cmp ecx,[last]
    append_le32(cave, static_cast<uint32_t>(flag_last_addr));
    cave.push_back(0x74);  // je .noedge
    emit8(0);
    cave.push_back(0x89);
    cave.push_back(0x0D);  // mov [last],ecx
    append_le32(cave, static_cast<uint32_t>(flag_last_addr));
    cave.push_back(0x83);
    cave.push_back(0xF9);  // cmp ecx,value
    cave.push_back(static_cast<unsigned char>(bag_flag_value & 0xFF));
    cave.push_back(0x75);  // jne .noedge
    emit8(0);
    cave.push_back(0xC7);
    cave.push_back(0x05);  // mov dword [arm],1
    append_le32(cave, static_cast<uint32_t>(flag_arm_addr));
    cave.push_back(0x01);
    cave.push_back(0x00);
    cave.push_back(0x00);
    cave.push_back(0x00);
    cave.push_back(0xE9);  // jmp .skip (rel32)
    emit32(1);
    // .noedge:
    noedge_label = cave.size();
    cave.push_back(0x83);
    cave.push_back(0x3D);  // cmp dword [arm],0
    append_le32(cave, static_cast<uint32_t>(flag_arm_addr));
    cave.push_back(0x00);
    cave.push_back(0x74);  // je .skip
    emit8(1);
    cave.push_back(0xC7);
    cave.push_back(0x05);  // mov dword [arm],0
    append_le32(cave, static_cast<uint32_t>(flag_arm_addr));
    cave.push_back(0x00);
    cave.push_back(0x00);
    cave.push_back(0x00);
    cave.push_back(0x00);
  }
  size_t capture_start = cave.size();
  cave.insert(cave.end(), capture_code.begin(), capture_code.end());
  size_t skip = capture_start + capt_end;
  for (auto& e : jmp8) {
    size_t target = (e.second == 0) ? noedge_label : skip;
    cave[e.first] = static_cast<unsigned char>(((target - (e.first + 1)) & 0xFF));
  }
  for (auto& e : jmp32) {
    size_t rel = skip - (e.first + 4);
    cave[e.first + 0] = static_cast<unsigned char>((rel) & 0xFF);
    cave[e.first + 1] = static_cast<unsigned char>((rel >> 8) & 0xFF);
    cave[e.first + 2] = static_cast<unsigned char>((rel >> 16) & 0xFF);
    cave[e.first + 3] = static_cast<unsigned char>((rel >> 24) & 0xFF);
  }
  cave.push_back(0x8B);
  cave.push_back(0x44);  // mov eax,[esi+eax*4+disp]
  cave.push_back(0x86);
  cave.push_back(disp & 0xFF);
  cave.push_back(0x5E);  // pop esi
  uint32_t rel = static_cast<uint32_t>(return_addr - (cave_addr + cave.size() + 5));
  cave.push_back(0xE9);  // jmp returnhere
  append_le32(cave, rel);
  return cave;
}

uintptr_t GameModder::find_cur_item_slot(uintptr_t hook_addr) {
  // 从已注入的 jmp 解析 cave,定位 "mov [cur_item],esi"(89 35 <addr>)中的 cur_item 槽地址。
  auto jmp = mem_.read(hook_addr, 5);
  if (jmp[0] != 0xE9) return 0;
  int32_t rel = static_cast<int32_t>(jmp[1]) | (static_cast<int32_t>(jmp[2]) << 8) |
                (static_cast<int32_t>(jmp[3]) << 16) | (static_cast<int32_t>(jmp[4]) << 24);
  uintptr_t cave_addr = static_cast<uintptr_t>((hook_addr + 5 + rel) & 0xFFFFFFFFULL);
  std::vector<unsigned char> cave;
  try {
    cave = mem_.read(cave_addr, 128);
  } catch (...) {
    return 0;
  }
  for (size_t i = 0; i + 6 <= cave.size(); ++i) {
    if (cave[i] == 0x89 && cave[i + 1] == 0x35) {
      uint32_t addr =
          static_cast<uint32_t>(cave[i + 2]) | (static_cast<uint32_t>(cave[i + 3]) << 8) |
          (static_cast<uint32_t>(cave[i + 4]) << 16) | (static_cast<uint32_t>(cave[i + 5]) << 24);
      if (addr > 0x10000 && addr < 0xFFFFFF00) return addr;
    }
  }
  return 0;
}

}  // namespace poq
