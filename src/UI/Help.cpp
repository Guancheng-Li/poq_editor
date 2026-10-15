// 秦殇物品修改器 (PoqEditor) —— 版本号/启动免责声明/消息弹窗队列/使用说明弹窗
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
            if (__DATE__[j] != names[i][j]) {
                eq = false;
                break;
            }
        if (eq) {
            m = i + 1;
            break;
        }
    }
    return m;
}
constexpr int kVerYear2() {
    return (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0');
}
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
    if (ImGui::BeginPopupModal(
            "关于", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
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
    if (ImGui::BeginPopupModal(
            popup_id.c_str(), nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
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
// ============================================================================
// 使用说明弹窗:多 tab,tab 名与主界面一致;打开时自动跳到当前所在 tab。
// (内存查看/偏移设置为调试 tab,无使用说明)
// ============================================================================
struct HelpTabContent {
    const char* name;          // tab 名(与主界面一致)
    const char* const* steps;  // 步骤文本,每行一条
    int step_count;
};

const char* const kHelpCharSteps[] = {
    "1. 初始化:启动游戏并进入存档后,点顶部\"初始化\"打开游戏进程。",
    "2. 读取属性:在\"角色\"下拉框选择要修改的人物,点\"读取属性\",网格中显示该人物的属性。",
    "3. "
    "修改:直接编辑网格中的各属性值;\"全局属性\"区为全队共享——金钱填好后点\"修改金钱\"写入,无敌模式/"
    "一击必杀勾选即生效。",
    "4. 保存属性:点\"保存属性\"把人物属性写回游戏内存。",
    "5. 存档:在游戏里存档,读档后可查看修改后的属性。",
};

const char* const kHelpItemSteps[] = {
    "1. 初始化:点\"初始化\"打开游戏进程(自动完成物品捕获,无需额外操作)。",
    "2. "
    "钩住物品:在游戏里把要改的物品从物品栏中捡起来(注意必须从物品栏捡起,地上捡起无效),"
    "钩子会锁定它。",
    "3. "
    "读取属性:点\"读取属性\",表格显示当前所有词条(同时自动缓存一份,供\"复制上一件物品\"使用);"
    "读不到或读到旧物品时,重复第 2 步再点读取。",
    "4. "
    "编辑词条:直接修改数值、颜色、类型,或用\"添加/删除/"
    "清空\"调整数量;也可\"从模板读取\"、\"另存为\"批量处理。",
    "5. "
    "保存属性:点\"保存属性\"写回内存。现有词条写回原链表节点;新增的词条由编辑器自动从节点池("
    "编辑器管理的一块固定内存)分配并接到链尾,无需手工干预。",
    "6. 游戏内生效:保存后在游戏里把该物品扔到地上再捡起来,修改才会真正生效。",
    "7. "
    "写入存档:确认生效后在游戏里存档,词条数值随物品一起写入存档文件,"
    "之后读档物品由游戏用原生内存重建,不再依赖编辑器。",
    "8. "
    "复制上一件物品:改好一把并保存后,捡起新物品,不用点读取,直接点\"复制上一件物品\","
    "上一件的词条会原样盖到新物品上(基础属性不复制),然后回到第 6 步生效。",
    "9. "
    "重置节点池(日常不用点):每次读档后点一次最保险,回收全部槽位、零风险;"
    "看到\"节点池无空闲槽\"提示时,先确认改过的物品都已扔下捡回,再点重置,然后重新保存。注意:"
    "背包里还有带新增词条的物品时不要点,会废掉那些词条。",
};

const char* const kHelpSummonSteps[] = {
    "1. 版本差异:Steam 版支持较好,可召唤更多具体类别、攻击力普遍更强(刺客除外);v1.30 "
    "只有部分功能——不支持召唤朱厌、善翼、刺客和指定类型的秦兵,"
    "且召唤物攻击力弱、无法造成伤害、只能拉仇恨,但秦兵死亡有物品掉落。",
    "2. 选择目标:选择要召唤的类别和具体类型(阵营可选队友/盟友),点\"激活\"。",
    "3. 召唤:激活后,在游戏里让力士使用\"烽火狼烟\",即可召唤出对应怪物。",
    "4. 取消:激活状态下点\"取消\",恢复烽火狼烟默认值(召唤狼)。",
    "5. 读档后:需要重新点\"激活\",或点\"刷新召唤\"。",
    "6. 清理召唤物:先用 GM 命令 look 查到目标 ID。队友用 dismiss {ID} "
    "解散(推荐,直接离队,无副作用);盟友不适用 dismiss,用 kill {ID} 逐个击杀(盟友太多可用 kill all "
    "清除,但可能误杀 NPC 甚至破坏剧情,建议逐个清理)。操作前先存档并备份 "
    "System\\game\\player_{i}.gam。",
};

const char* const kHelpGmSteps[] = {
    "1. 激活:点\"激活\"按钮获取 GM 权限;一种方式无效时可尝试另一种,或按第 2 "
    "条手动激活(程序会静默建立 wizard.lst 文件并加入当前用户)。",
    "2. 手动激活方法:在\"游戏目录\\System\\wizard\\wizard.lst\"里加入\"9 {用户名}\"(单机用户名为 "
    "player,文件或目录不存在可自行创建),重启游戏,在游戏中按回车依次输入\"{$objectpoq$}\"和\"r "
    "9\",屏幕出现激活提示。",
    "3. 执行命令:激活后在输入框输入 GM 命令,按回车或点\"执行\"按钮执行(工具内可以无视 GM "
    "命令长度限制,但过长是否会崩溃没试过);可勾选\"执行次数\"批量执行。",
    "4. 历史命令:输入框内按上箭头向前翻最近执行过的命令(最多缓存 30 "
    "条,执行成功才入历史),下箭头往回翻;"
    "下翻翻过最新一条会还原浏览前正在输入的内容。焦点在常用命令列表时,上下箭头留给列表不切历史。",
    "5. 命令技巧:GM "
    "命令可参考网上攻略,例如杀死杂货铺掌柜或铁匠铺掌柜获得装备,杀完后用创建命令恢复(名字可以用 "
    "look 看到)。",
    "6. 常用命令:放在工具目录的 gm_cmd.txt 中(每行\"命令#显示名\",# "
    "后为显示名可省略),会出现在\"常用命令\"下拉里供快速输入,文件可自行编辑。",
};

const char* const kHelpH2Steps[] = {
    "1. "
    "用途:游戏参数(如物品爆率)以加密形式存放在\"游戏目录\\Data\\\"下,本工具对这些文件原地解密/"
    "加密。",
    "2. 解密:选择 Data 目录下的加密文件点\"解密\";转换前自动备份原文件为 "
    "<名>.<yyyymmddhhMMss>.bak.<扩展名>。",
    "3. 编辑:解密后的文件是 GBK 编码的表格,请用 Excel "
    "打开、编辑、保存;修改方法可参考论坛或其他教程。",
    "4. "
    "加密:编辑完成后点\"加密\"转回加密格式,同样会先备份(加密/"
    "解密都备份);改错了可以用备份文件替换掉修改错误的文件恢复。",
    "5. 生效:修改后需要退出游戏重新进入才生效(存档+读档是否生效未确定)。",
};

const char* const kHelpGamSteps[] = {
    "1. 用途:直接编辑 .gam 存档文件里的物品(文件级,不依赖游戏内存/注入)。",
    "2. 打开:点\"打开 gam\"选择存档(游戏运行时默认进 System\\game "
    "目录)。左侧树按\"地图→物品\"两级展开。打开时会自动备份原文件为 "
    "<名>.<yyyymmddhhMMss>.bak.gam。",
    "3. 选中:点物品名字选中它(右侧进入编辑);点地图/房间名选中该容器。上方\"记录长度 N "
    "字节\"是该条记录的净内容大小。",
    "4. "
    "词条表:一行一个属性。第一列可勾选(配\"删除选中\"批量删);"
    "\"添加属性\"加一行。属性名下拉框与\"道具修改\"同一张表;颜色/"
    "类型(数值·百分比)也在表里。上限是独立的一行——例如\"普通攻击\"配一行\"最大普通攻击\","
    "保存时会自动合成回存档的 \"名 +值 ~ +上限\" 格式。",
    "5. "
    "改值:内容变长或变短都由保存时自动重排(单/"
    "多地图存档均支持);若改短了,"
    "保存时会自动把记录补回原长度——因为内容变短会让游戏里地上的物品消失。",
    "6. "
    "增删物品:物品行有\"删除\"(删除本件)、\"复制\"(复制本件记录);地图节点有\"删除\"("
    "删除该地图下全部物品)、\"粘贴\"(把复制的物品粘贴到该地图);"
    "也可用\"创建物品\"从预置目录按类别→名字新建。",
    "7. 复制属性/粘贴属性:粘贴时会弹框让你选——名字/别名/类型 "
    "要不要一起粘贴。选\"只复制属性\"则这三项保持原样,其余属性照常粘贴;"
    "选\"取消复制\"则什么都不做。",
    "8. 保存:点\"保存文件\"写回(备份在打开时已自动生成,无需手动)。",
    "9. 非装备记录(技能等):词条里带 0x10x/0x11x 这类\"装备表之外\"id "
    "的记录会标成[非装备记录],属性编辑只读——避免改写时丢掉它的词条。",
    "10. 备份清理:关闭窗口时会问一次——是否按 MD5 "
    "校验清理掉\"本次打开且没改动过\"的备份文件(校验要读文件,可能卡顿几秒;改过的备份一定会保留)"
    "。可点\"取消退出\"留在程序里。",
    "11. "
    "注意:工具读的是文件。若刚在游戏里改过物品(打造/"
    "鉴定等),必须先在游戏里存档(F9),再在工具里重新\"打开 "
    "gam\"才能看到最新数据;在工具里改完存档文件后,进游戏读档生效。",
};

void RenderHelpPopup(App& app) {
    static const HelpTabContent kHelpTabs[] = {
        {"人物修改", kHelpCharSteps, 5},   {"道具修改", kHelpItemSteps, 9},
        {"召唤修改", kHelpSummonSteps, 6}, {"GM命令", kHelpGmSteps, 6},
        {"H2解密", kHelpH2Steps, 5},       {"gam修改", kHelpGamSteps, 11},
    };
    // 固定尺寸(不随 tab 内容变化), 超出屏幕时收窄; 仍然居中、不可拖动、不可缩放
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    float hw = 820.0f, hh = 620.0f;
    if (hw > vp.x - 80.0f) hw = vp.x - 80.0f;
    if (hh > vp.y - 80.0f) hh = vp.y - 80.0f;
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(hw, hh), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal(
            "帮助", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse)) {
        return;
    }
    // 底部按钮行占的高度(正文区按它留白, 按钮固定在弹窗底部不随内容滚动)
    const float footer_h = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    if (ImGui::BeginTabBar("helptabs")) {
        const int kHelpTabCount = (int)(sizeof(kHelpTabs) / sizeof(kHelpTabs[0]));
        for (int i = 0; i < kHelpTabCount; ++i) {
            // 打开后的第一帧把当前主 tab 对应的说明设为选中,之后可自由切换。
            // help_tab 用主 tab 序号(0人物 1道具 2召唤 3GM 4H2 5gam); 调试 tab(6/7)没有说明页,
            // 不跳。
            int want = (app.help_tab >= 0 && app.help_tab < kHelpTabCount) ? app.help_tab : -1;
            ImGuiTabItemFlags flags =
                (app.help_jump && i == want) ? ImGuiTabItemFlags_SetSelected : 0;
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

}  // namespace poq
