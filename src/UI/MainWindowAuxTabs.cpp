// 主窗口 —— F12 调试 Tab：内存查看 / 偏移设置。

#include <cstdio>
#include <string>
#include <vector>
#include "imgui.h"
#include "UI/App.h"
#include "UI/AppCfgState.h"
#include "UI/AppItemState.h"

namespace poq {

void render_mem_tab(App& app) {
  ImGui::Text("地址(0x...)");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(180);
  ImGui::InputText("##addr", app.item().raw_addr, sizeof(app.item().raw_addr));
  ImGui::SameLine(0, 20);
  ImGui::Text("长度");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70);
  ImGui::InputText("##size", app.item().raw_size, sizeof(app.item().raw_size));
  ImGui::SameLine(0, 20);
  if (ImGui::Button("读取")) on_raw_read(app);
  ImGui::SameLine();
  if (ImGui::Button("写入")) on_raw_write(app);

  ImGui::Separator();
  float avail_y = ImGui::GetContentRegionAvail().y;
  if (avail_y > 40) avail_y -= 26;  // 给说明留一行
  ImGui::InputTextMultiline("##hex", app.item().raw_hex, sizeof(app.item().raw_hex),
                            ImVec2(-FLT_MIN, avail_y), ImGuiInputTextFlags_None);
  ImGui::TextDisabled("查看拾取物品指针附近内存。写入:每行 \"地址: 十六进制字节\" 或纯字节均可。");
}

void render_cfg_tab(App& app) {
  // 模式选择
  std::vector<std::string> mode_labels;
  for (const auto& kv : app.cfg().cfg.modes)
    mode_labels.push_back(kv.first + "：" + kv.second.name);
  std::vector<const char*> mode_items;
  for (const auto& s : mode_labels) mode_items.push_back(s.c_str());
  int sel = app.cfg().mode_selected;
  ImGui::Text("模式:");
  ImGui::SameLine();
  if (ImGui::Combo("##mode", &sel, mode_items.data(), static_cast<int>(mode_items.size()))) {
    app.cfg().mode_selected = sel;
  }

  auto row = [&](const char* label, char* buf, size_t buflen) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(300);
    ImGui::InputText(("##cfg" + std::string(label)).c_str(), buf, buflen);
  };

  ImGui::Separator();
  row("注入模块 (hook.module)", app.cfg().cfg_module, sizeof(app.cfg().cfg_module));
  row("注入偏移 (hook.offset, 0x...)", app.cfg().cfg_offset, sizeof(app.cfg().cfg_offset));
  row("原指令字节 (hook.aob)", app.cfg().cfg_aob, sizeof(app.cfg().cfg_aob));
  row("重量偏移 (hook.weight_offset, 0x...)", app.cfg().cfg_weight_offset,
      sizeof(app.cfg().cfg_weight_offset));
  row("重量上限 (hook.weight_max)", app.cfg().cfg_weight_max, sizeof(app.cfg().cfg_weight_max));
  row("物品基址调整 (item_base_adjust)", app.cfg().cfg_item_adjust,
      sizeof(app.cfg().cfg_item_adjust));
  row("属性头偏移 (attr_list.head_offset, 0x...)", app.cfg().cfg_head, sizeof(app.cfg().cfg_head));
  row("节点 next 偏移 (0x...)", app.cfg().cfg_next, sizeof(app.cfg().cfg_next));
  row("节点 ID 偏移 (0x...)", app.cfg().cfg_id, sizeof(app.cfg().cfg_id));
  row("节点 color 偏移 (0x...)", app.cfg().cfg_color, sizeof(app.cfg().cfg_color));
  row("节点 precent 偏移 (0x...)", app.cfg().cfg_precent, sizeof(app.cfg().cfg_precent));
  row("节点 value 偏移 (0x...)", app.cfg().cfg_value, sizeof(app.cfg().cfg_value));

  ImGui::Separator();
  if (ImGui::Button("保存配置并应用")) on_save_cfg(app);
  ImGui::TextDisabled(
      "说明:以上为当前模式的 magic number,可在 poq 不同版本间切换。修改后点击\"保存配置并应用\""
      "立即生效,但仅本次会话有效(单文件版不写配置文件,重启回到内置默认)。"
      "内置默认在 src/Core/OffsetsConfig.cpp 的 default_mode() 中维护,"
      "需要固定偏移时改代码重新编译。");
}
}  // namespace poq
