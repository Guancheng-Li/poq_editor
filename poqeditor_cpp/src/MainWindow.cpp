#include "MainWindow.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <QChar>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include "Core/IniTemplate.h"

namespace poq {

namespace {

// Delete all widgets owned by a layout (used when rebuilding a panel).
void ClearLayout(QLayout* layout) {
    if (layout == nullptr) return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) delete w;
        delete item;
    }
}

// Parse a decimal int, returning false on failure (mirrors C# int.TryParse).
bool TryParseInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        int v = std::stoi(s, &pos);
        if (pos != s.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

bool TryParseUInt(const std::string& s, unsigned int& out) {
    if (s.empty()) return false;
    try {
        size_t pos = 0;
        unsigned long v = std::stoul(s, &pos);
        if (pos != s.size()) return false;
        out = static_cast<unsigned int>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool TryParseBool(const std::string& s, bool& out) {
    std::string t;
    t.reserve(s.size());
    for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "true") { out = true; return true; }
    if (t == "false") { out = false; return true; }
    return false;
}

}  // namespace

// ============================================================================
// Construction / teardown
// ============================================================================

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromUtf8("秦殇内存修改器 (PoqEditor) - 64位重制版"));
    resize(1100, 720);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);

    // --- Top: process selection ---
    auto* proc_group = new QGroupBox(QString::fromUtf8("进程"), central);
    auto* proc_row = new QHBoxLayout(proc_group);
    proc_row->addWidget(new QLabel(QString::fromUtf8("选择进程："), proc_group));
    cb_process_ = new QComboBox(proc_group);
    cb_process_->setEditable(true);
    cb_process_->setMinimumWidth(320);
    proc_row->addWidget(cb_process_, 1);
    btn_init_ = new QPushButton(QString::fromUtf8("初始化"), proc_group);
    QFont bold_font = btn_init_->font();
    bold_font.setBold(true);
    btn_init_->setFont(bold_font);
    proc_row->addWidget(btn_init_);
    btn_refresh_ = new QPushButton(QString::fromUtf8("刷新进程"), proc_group);
    proc_row->addWidget(btn_refresh_);
    root->addWidget(proc_group);

    // --- Tabs ---
    auto* tabs = new QTabWidget(central);
    root->addWidget(tabs, 1);

    // Tab 1: 物品修改
    {
        auto* tab = new QWidget(tabs);
        auto* v = new QVBoxLayout(tab);

        auto* basic_gb = new QGroupBox(QString::fromUtf8("基础属性"), tab);
        basic_fields_panel_ = new QWidget(basic_gb);
        new QGridLayout(basic_fields_panel_);  // populated by BuildBasicFieldsPanel
        auto* basic_l = new QVBoxLayout(basic_gb);
        basic_l->addWidget(basic_fields_panel_);
        v->addWidget(basic_gb);

        auto* list_gb = new QGroupBox(QString::fromUtf8("属性列表（46 条模板）"), tab);
        auto* list_l = new QVBoxLayout(list_gb);
        auto* btns = new QHBoxLayout();
        auto* btn_read = new QPushButton(QString::fromUtf8("读取属性"), list_gb);
        btn_read->setFont(bold_font);
        auto* btn_load_tpl = new QPushButton(QString::fromUtf8("从模板读取"), list_gb);
        auto* btn_save = new QPushButton(QString::fromUtf8("保存属性"), list_gb);
        btn_save->setFont(bold_font);
        auto* btn_add = new QPushButton(QString::fromUtf8("添加"), list_gb);
        auto* btn_del = new QPushButton(QString::fromUtf8("删除"), list_gb);
        auto* btn_clear = new QPushButton(QString::fromUtf8("清空"), list_gb);
        btns->addWidget(btn_read);
        btns->addWidget(btn_load_tpl);
        btns->addWidget(btn_save);
        btns->addWidget(btn_add);
        btns->addWidget(btn_del);
        btns->addWidget(btn_clear);
        btns->addStretch(1);
        lb_count_ = new QLabel(QString::fromUtf8("属性数：0"), list_gb);
        btns->addWidget(lb_count_);
        list_l->addLayout(btns);

        prop_grid_ = new QTableWidget(list_gb);
        prop_grid_->setColumnCount(5);
        prop_grid_->setHorizontalHeaderLabels(QStringList()
            << QString::fromUtf8("地址(写入位置)")
            << QString::fromUtf8("Index(属性代码)")
            << QString::fromUtf8("Num(数值)")
            << QString::fromUtf8("Color(颜色)")
            << QString::fromUtf8("Precent(百分比)"));
        prop_grid_->setSelectionBehavior(QAbstractItemView::SelectRows);
        prop_grid_->setEditTriggers(QAbstractItemView::DoubleClicked |
                                    QAbstractItemView::EditKeyPressed |
                                    QAbstractItemView::AnyKeyPressed);
        prop_grid_->horizontalHeader()->setStretchLastSection(true);
        prop_grid_->setColumnWidth(0, 140);
        prop_grid_->setColumnWidth(1, 120);
        prop_grid_->setColumnWidth(2, 120);
        prop_grid_->setColumnWidth(3, 110);
        prop_grid_->setColumnWidth(4, 120);
        list_l->addWidget(prop_grid_);
        v->addWidget(list_gb, 1);

        tabs->addTab(tab, QString::fromUtf8("物品修改"));

        connect(btn_read, &QPushButton::clicked, this, &MainWindow::OnReadProperties);
        connect(btn_load_tpl, &QPushButton::clicked, this, &MainWindow::OnLoadTemplate);
        connect(btn_save, &QPushButton::clicked, this, &MainWindow::OnSaveProperties);
        connect(btn_add, &QPushButton::clicked, this, &MainWindow::OnAddProperty);
        connect(btn_del, &QPushButton::clicked, this, &MainWindow::OnDeleteProperty);
        connect(btn_clear, &QPushButton::clicked, this, &MainWindow::OnClearProperties);
    }

    // Tab 2: 内存查看
    {
        auto* tab = new QWidget(tabs);
        auto* v = new QVBoxLayout(tab);

        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(QString::fromUtf8("地址(0x...)"), tab));
        raw_addr_ = new QLineEdit(QString::fromUtf8("0x901FF0"), tab);
        raw_addr_->setMaximumWidth(160);
        row->addWidget(raw_addr_);
        auto* len_lbl = new QLabel(QString::fromUtf8("长度"), tab);
        len_lbl->setContentsMargins(8, 0, 0, 0);
        row->addWidget(len_lbl);
        raw_size_ = new QLineEdit(QString::fromUtf8("64"), tab);
        raw_size_->setMaximumWidth(60);
        row->addWidget(raw_size_);
        auto* btn_raw_read = new QPushButton(QString::fromUtf8("读取"), tab);
        auto* btn_raw_write = new QPushButton(QString::fromUtf8("写入"), tab);
        row->addWidget(btn_raw_read);
        row->addWidget(btn_raw_write);
        row->addStretch(1);
        v->addLayout(row);

        raw_hex_ = new QPlainTextEdit(tab);
        QFont mono("Consolas");
        mono.setStyleHint(QFont::Monospace);
        raw_hex_->setFont(mono);
        raw_hex_->setLineWrapMode(QPlainTextEdit::NoWrap);
        v->addWidget(raw_hex_, 1);

        auto* note = new QLabel(
            QString::fromUtf8("写入时：上面 hex 文本框里每行 \"地址: 十六进制字节\" "
                              "或纯十六进制字节均可，程序会解析后写入。读取用于校准偏移。"),
            tab);
        note->setStyleSheet("color: gray;");
        note->setWordWrap(true);
        v->addWidget(note);

        tabs->addTab(tab, QString::fromUtf8("内存查看"));

        connect(btn_raw_read, &QPushButton::clicked, this, &MainWindow::OnRawRead);
        connect(btn_raw_write, &QPushButton::clicked, this, &MainWindow::OnRawWrite);
    }

    // Tab 3: 偏移配置
    {
        auto* tab = new QWidget(tabs);
        auto* v = new QVBoxLayout(tab);

        auto* scroll = new QScrollArea(tab);
        scroll->setWidgetResizable(true);
        cfg_panel_ = new QWidget(scroll);
        new QVBoxLayout(cfg_panel_);  // populated by BuildConfigPanel
        scroll->setWidget(cfg_panel_);
        v->addWidget(scroll);

        tabs->addTab(tab, QString::fromUtf8("偏移配置"));
    }

    setCentralWidget(central);

    // --- Status bar ---
    status_text_ = new QLabel(this);
    statusBar()->addWidget(status_text_, 1);

    // --- Wire top-bar buttons ---
    connect(btn_refresh_, &QPushButton::clicked, this, &MainWindow::OnRefreshProcesses);
    connect(btn_init_, &QPushButton::clicked, this, &MainWindow::OnInit);

    // --- Initial load (mirrors C# Window_Loaded) ---
    ReloadConfig();
    RefreshProcesses();
    Status(QString::fromUtf8("就绪。请以管理员身份运行，并在游戏中拾起要修改的物品。"));
}

MainWindow::~MainWindow() = default;

// ============================================================================
// Paths
// ============================================================================

QString MainWindow::AppDir() {
    return QCoreApplication::applicationDirPath();
}

QString MainWindow::CfgPath() {
    return AppDir() + QDir::separator() + QString::fromUtf8("offsets.json");
}

// ============================================================================
// Config reload / panels
// ============================================================================

void MainWindow::ReloadConfig() {
    cfg_ = OffsetsConfig::Load(CfgPath());
    modder_ = std::make_unique<GameModder>(cfg_);
    BuildBasicFieldsPanel();
    BuildConfigPanel();
}

void MainWindow::BuildBasicFieldsPanel() {
    auto* grid = qobject_cast<QGridLayout*>(basic_fields_panel_->layout());
    if (grid == nullptr) {
        grid = new QGridLayout(basic_fields_panel_);
    }
    ClearLayout(grid);
    basic_boxes_.clear();

    const int kCols = 4;
    int row = 0, col = 0;
    for (const auto& f : cfg_.basic_fields) {
        auto* cell = new QWidget(basic_fields_panel_);
        auto* h = new QHBoxLayout(cell);
        h->setContentsMargins(6, 3, 6, 3);
        auto* lbl = new QLabel(
            QString::fromUtf8(f.label.c_str()) +
                QString(" (+0x%1)").arg(f.offset, 0, 16).toUpper(),
            cell);
        auto* edit = new QLineEdit(QString::fromUtf8("0"), cell);
        edit->setMaximumWidth(90);
        edit->setMinimumWidth(60);
        h->addWidget(lbl);
        h->addWidget(edit);
        cell->setStyleSheet("border: 1px solid lightgray;");
        grid->addWidget(cell, row, col);
        basic_boxes_[f.label] = edit;
        ++col;
        if (col >= kCols) { col = 0; ++row; }
    }
}

void MainWindow::BuildConfigPanel() {
    auto* v = qobject_cast<QVBoxLayout*>(cfg_panel_->layout());
    if (v == nullptr) {
        v = new QVBoxLayout(cfg_panel_);
    }
    ClearLayout(v);
    cfg_boxes_.clear();

    auto add_row = [&](const QString& label, const QString& value, const std::string& key) {
        auto* row_w = new QWidget(cfg_panel_);
        auto* h = new QHBoxLayout(row_w);
        h->setContentsMargins(2, 2, 2, 2);
        auto* lbl = new QLabel(label, row_w);
        lbl->setMinimumWidth(260);
        lbl->setMaximumWidth(260);
        auto* edit = new QLineEdit(value, row_w);
        edit->setMaximumWidth(240);
        edit->setMinimumWidth(120);
        h->addWidget(lbl);
        h->addWidget(edit);
        h->addStretch(1);
        v->addWidget(row_w);
        cfg_boxes_[key] = edit;
    };

    add_row(QString::fromUtf8("锚点地址 (AnchorAddress)"),
            QString::fromUtf8(cfg_.anchor_address.c_str()), "AnchorAddress");
    add_row(QString::fromUtf8("物品基址调整 (ItemBaseAdjust, -0xF0=-240)"),
            QString::number(cfg_.item_base_adjust), "ItemBaseAdjust");
    add_row(QString::fromUtf8("进程访问权限 (ProcessAccess)"),
            QString::fromUtf8(cfg_.process_access.c_str()), "ProcessAccess");
    add_row(QString::fromUtf8("属性数组起点 (BaseOffsetFromItem)"),
            QString::number(cfg_.property_list.base_offset_from_item), "BaseOffsetFromItem");
    add_row(QString::fromUtf8("属性数量 (Count)"),
            QString::number(cfg_.property_list.count), "Count");
    add_row(QString::fromUtf8("属性槽步长 (Stride)"),
            QString::number(cfg_.property_list.stride), "Stride");
    add_row(QString::fromUtf8("Index 偏移"),
            QString::number(cfg_.property_list.index_offset), "IndexOffset");
    add_row(QString::fromUtf8("Num 偏移"),
            QString::number(cfg_.property_list.num_offset), "NumOffset");
    add_row(QString::fromUtf8("Color 偏移"),
            QString::number(cfg_.property_list.color_offset), "ColorOffset");
    add_row(QString::fromUtf8("Precent 偏移"),
            QString::number(cfg_.property_list.precent_offset), "PrecentOffset");
    add_row(QString::fromUtf8("读取时扫描数组 (ScanArray, true/false)"),
            cfg_.property_list.scan_array ? QString::fromUtf8("true") : QString::fromUtf8("false"),
            "ScanArray");

    auto* btn_save_cfg = new QPushButton(QString::fromUtf8("保存配置并应用"), cfg_panel_);
    btn_save_cfg->setContentsMargins(2, 12, 2, 2);
    v->addWidget(btn_save_cfg);

    auto* note = new QLabel(
        QString::fromUtf8("说明：以上偏移来自对原版 poq_hack.exe 的逆向，默认值可能需要针对实际 "
                          "POQ.EXE v1.30 校准。\n"
                          "可用“内存查看”页读取物品内存来核对。修改后点击“保存配置并应用”即写入 "
                          "offsets.json 并即时生效。"),
        cfg_panel_);
    note->setStyleSheet("color: gray;");
    note->setWordWrap(true);
    v->addWidget(note);
    v->addStretch(1);

    connect(btn_save_cfg, &QPushButton::clicked, this, &MainWindow::OnSaveConfig);
}

// ============================================================================
// Process selection
// ============================================================================

void MainWindow::OnRefreshProcesses() { RefreshProcesses(); }

void MainWindow::RefreshProcesses() {
    processes_ = ProcessMemory::EnumerateProcesses();
    cb_process_->clear();
    int poq_index = -1;
    for (int i = 0; i < static_cast<int>(processes_.size()); ++i) {
        const ProcessInfo& p = processes_[i];
        cb_process_->addItem(p.Display(), QVariant(p.pid));
        // Case-insensitive match for POQ.EXE.
        std::string name = p.name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name == "poq.exe") poq_index = i;
    }
    if (poq_index >= 0) cb_process_->setCurrentIndex(poq_index);

    QString msg = QString::fromUtf8("已列出 %1 个进程。").arg(processes_.size());
    if (poq_index >= 0)
        msg += QString::fromUtf8("已自动选中 POQ.EXE。");
    else
        msg += QString::fromUtf8("未找到 POQ.EXE，请手动选择或启动游戏。");
    Status(msg);
}

bool MainWindow::TryGetSelectedPid(unsigned int& pid) {
    // Selected item?
    QVariant data = cb_process_->currentData();
    if (data.isValid() && data.canConvert<unsigned int>()) {
        bool ok = false;
        unsigned int v = data.toUInt(&ok);
        if (ok) { pid = v; return true; }
    }
    // User-typed numeric PID?
    std::string txt = cb_process_->currentText().toStdString();
    unsigned int v = 0;
    if (TryParseUInt(txt, v)) { pid = v; return true; }
    // Match by name.
    for (const auto& p : processes_) {
        std::string a = p.name;
        std::string b = txt;
        std::transform(a.begin(), a.end(), a.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (a == b) { pid = p.pid; return true; }
    }
    return false;
}

void MainWindow::OnInit() {
    unsigned int pid = 0;
    if (!TryGetSelectedPid(pid)) {
        Status(QString::fromUtf8("请先选择一个进程。"));
        return;
    }
    try {
        uintptr_t ptr = modder_->Initialize(pid);
        uintptr_t base = modder_->GetItemBase();
        Status(QString::fromUtf8("初始化成功。手持指针 = 0x%1（物品基址 = 0x%2）。")
                   .arg(static_cast<qulonglong>(ptr), 8, 16, QChar('0'))
                   .arg(static_cast<qulonglong>(base), 8, 16, QChar('0')));
    } catch (const std::exception& ex) {
        Status(QString::fromUtf8("初始化失败：") + QString::fromUtf8(ex.what()));
    }
}

// ============================================================================
// Property read / load / save / add / del / clear
// ============================================================================

void MainWindow::OnReadProperties() {
    if (!modder_->Mem().IsOpen()) {
        Status(QString::fromUtf8("请先点击“初始化”。"));
        return;
    }
    try {
        uintptr_t ib = modder_->GetItemBase();
        // Basic fields.
        auto basic = modder_->ReadBasicFields(ib);
        for (const auto& kv : basic) {
            auto it = basic_boxes_.find(kv.first);
            if (it != basic_boxes_.end())
                it->second->setText(QString::number(kv.second));
        }
        // Property list.
        auto props = modder_->ReadProperties(ib);
        prop_grid_->setRowCount(0);
        for (const ItemProperty& p : props) {
            int r = prop_grid_->rowCount();
            prop_grid_->insertRow(r);
            prop_grid_->setItem(r, 0, new QTableWidgetItem(QString::fromUtf8(p.address.c_str())));
            prop_grid_->setItem(r, 1, new QTableWidgetItem(QString::number(p.index)));
            prop_grid_->setItem(r, 2, new QTableWidgetItem(QString::number(p.num)));
            prop_grid_->setItem(r, 3, new QTableWidgetItem(QString::number(p.color)));
            prop_grid_->setItem(r, 4, new QTableWidgetItem(QString::number(p.precent)));
        }
        UpdateCount();
        Status(QString::fromUtf8("读取完成。物品基址 = 0x%1，属性 %2 条。")
                   .arg(static_cast<qulonglong>(ib), 8, 16, QChar('0'))
                   .arg(prop_grid_->rowCount()));
    } catch (const std::exception& ex) {
        Status(QString::fromUtf8("读取失败：") + QString::fromUtf8(ex.what()));
    }
}

void MainWindow::OnLoadTemplate() {
    QString path = AppDir() + QDir::separator() + QString::fromUtf8(cfg_.template_ini.c_str());
    try {
        auto tpl = IniTemplate::Load(path);
        if (tpl.empty()) {
            Status(QString::fromUtf8("模板为空或文件不存在：") + path);
            return;
        }
        // If the grid already has addresses (read before), match template values
        // by Index; otherwise load the template directly.
        if (prop_grid_->rowCount() > 0 && AnyRowHasAddress()) {
            for (int r = 0; r < prop_grid_->rowCount(); ++r) {
                int row_index = prop_grid_->item(r, 1)->text().toInt();
                for (const ItemProperty& m : tpl) {
                    if (m.index == row_index) {
                        prop_grid_->item(r, 2)->setText(QString::number(m.num));
                        prop_grid_->item(r, 3)->setText(QString::number(m.color));
                        prop_grid_->item(r, 4)->setText(QString::number(m.precent));
                        break;
                    }
                }
            }
            Status(QString::fromUtf8("已按 Index 匹配模板数值到现有 %1 条属性。")
                       .arg(prop_grid_->rowCount()));
        } else {
            prop_grid_->setRowCount(0);
            for (const ItemProperty& p : tpl) {
                int r = prop_grid_->rowCount();
                prop_grid_->insertRow(r);
                prop_grid_->setItem(r, 0, new QTableWidgetItem(QString::fromUtf8(p.address.c_str())));
                prop_grid_->setItem(r, 1, new QTableWidgetItem(QString::number(p.index)));
                prop_grid_->setItem(r, 2, new QTableWidgetItem(QString::number(p.num)));
                prop_grid_->setItem(r, 3, new QTableWidgetItem(QString::number(p.color)));
                prop_grid_->setItem(r, 4, new QTableWidgetItem(QString::number(p.precent)));
            }
            Status(QString::fromUtf8("已从模板载入 %1 条属性（地址列为空，保存前请先“读取属性”或手填地址）。")
                       .arg(prop_grid_->rowCount()));
        }
        UpdateCount();
    } catch (const std::exception& ex) {
        Status(QString::fromUtf8("载入模板失败：") + QString::fromUtf8(ex.what()));
    }
}

void MainWindow::OnSaveProperties() {
    if (!modder_->Mem().IsOpen()) {
        Status(QString::fromUtf8("请先点击“初始化”。"));
        return;
    }
    if (prop_grid_->rowCount() == 0 || !AnyRowHasAddress()) {
        Status(QString::fromUtf8("没有可写入的属性地址。请先“读取属性”或在地址列填入地址。"));
        return;
    }
    try {
        uintptr_t ib = modder_->GetItemBase();
        // Basic fields.
        std::unordered_map<std::string, unsigned int> basic;
        for (const auto& kv : basic_boxes_) {
            unsigned int v = 0;
            if (TryParseUInt(kv.second->text().toStdString(), v)) basic[kv.first] = v;
        }
        modder_->WriteBasicFields(ib, basic);
        // Property list.
        auto props = CollectPropsFromTable();
        modder_->WriteProperties(props);
        int written = 0;
        for (const ItemProperty& p : props) if (!p.address.empty()) ++written;
        Status(QString::fromUtf8("保存完成：基础属性 %1 项，属性 %2 条已写入。"
                                 "请在游戏中把物品扔到地上再捡起使修改生效。")
                   .arg(basic.size())
                   .arg(written));
    } catch (const std::exception& ex) {
        Status(QString::fromUtf8("保存失败：") + QString::fromUtf8(ex.what()));
    }
}

void MainWindow::OnAddProperty() {
    int r = prop_grid_->rowCount();
    prop_grid_->insertRow(r);
    for (int c = 0; c < 5; ++c)
        prop_grid_->setItem(r, c, new QTableWidgetItem(QString::fromUtf8("0")));
    prop_grid_->item(r, 0)->setText(QString::fromUtf8(""));
    UpdateCount();
}

void MainWindow::OnDeleteProperty() {
    QList<QTableWidgetItem*> sel = prop_grid_->selectedItems();
    if (sel.isEmpty()) {
        Status(QString::fromUtf8("请先在表格里选中要删除的行。"));
        return;
    }
    // Collect unique row indices and remove from highest to lowest.
    std::set<int> rows;
    for (QTableWidgetItem* it : sel) rows.insert(it->row());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        prop_grid_->removeRow(*it);
    }
    UpdateCount();
}

void MainWindow::OnClearProperties() {
    prop_grid_->setRowCount(0);
    UpdateCount();
}

void MainWindow::UpdateCount() {
    lb_count_->setText(QString::fromUtf8("属性数：%1").arg(prop_grid_->rowCount()));
}

std::vector<ItemProperty> MainWindow::CollectPropsFromTable() {
    std::vector<ItemProperty> out;
    out.reserve(prop_grid_->rowCount());
    for (int r = 0; r < prop_grid_->rowCount(); ++r) {
        ItemProperty p;
        p.address = prop_grid_->item(r, 0) ? prop_grid_->item(r, 0)->text().toStdString() : "";
        // Trim whitespace.
        size_t a = p.address.find_first_not_of(" \t\r\n");
        if (a != std::string::npos) {
            size_t b = p.address.find_last_not_of(" \t\r\n");
            p.address = p.address.substr(a, b - a + 1);
        } else {
            p.address = "";
        }
        auto cell_int = [&](int col) -> int {
            QTableWidgetItem* it = prop_grid_->item(r, col);
            return it ? it->text().toInt() : 0;
        };
        p.index = cell_int(1);
        p.num = cell_int(2);
        p.color = cell_int(3);
        p.precent = cell_int(4);
        out.push_back(std::move(p));
    }
    return out;
}

bool MainWindow::AnyRowHasAddress() {
    for (int r = 0; r < prop_grid_->rowCount(); ++r) {
        QTableWidgetItem* it = prop_grid_->item(r, 0);
        if (it == nullptr) continue;
        QString t = it->text().trimmed();
        if (!t.isEmpty()) return true;
    }
    return false;
}

// ============================================================================
// Raw memory view
// ============================================================================

void MainWindow::OnRawRead() {
    if (!modder_->Mem().IsOpen()) {
        Status(QString::fromUtf8("请先“初始化”。"));
        return;
    }
    try {
        uintptr_t addr = ProcessMemory::ParseAddress(raw_addr_->text());
        int size = 64;
        bool ok = false;
        int parsed = raw_size_->text().toInt(&ok);
        if (ok && parsed > 0) size = parsed;
        auto data = modder_->Mem().Read(addr, size);
        raw_hex_->setPlainText(FormatHexDump(addr, data));
        Status(QString::fromUtf8("已读取 0x%1 共 %2 字节。")
                   .arg(static_cast<qulonglong>(addr), 8, 16, QChar('0'))
                   .arg(static_cast<int>(data.size())));
    } catch (const std::exception& ex) {
        Status(QString::fromUtf8("读取失败：") + QString::fromUtf8(ex.what()));
    }
}

void MainWindow::OnRawWrite() {
    if (!modder_->Mem().IsOpen()) {
        Status(QString::fromUtf8("请先“初始化”。"));
        return;
    }
    try {
        uintptr_t addr = ProcessMemory::ParseAddress(raw_addr_->text());
        auto bytes = ParseHexDump(raw_hex_->toPlainText());
        if (bytes.empty()) {
            Status(QString::fromUtf8("没有可写入的十六进制字节。"));
            return;
        }
        modder_->Mem().Write(addr, bytes);
        Status(QString::fromUtf8("已写入 0x%1 共 %2 字节。")
                   .arg(static_cast<qulonglong>(addr), 8, 16, QChar('0'))
                   .arg(static_cast<int>(bytes.size())));
    } catch (const std::exception& ex) {
        Status(QString::fromUtf8("写入失败：") + QString::fromUtf8(ex.what()));
    }
}

QString MainWindow::FormatHexDump(uintptr_t base_addr, const std::vector<unsigned char>& data) {
    QString out;
    for (size_t i = 0; i < data.size(); i += 16) {
        out += QString("%1: ")
                   .arg(static_cast<qulonglong>(base_addr + i), 8, 16, QChar('0'))
                   .toUpper();
        size_t n = std::min<size_t>(16, data.size() - i);
        for (size_t j = 0; j < n; ++j) {
            out += QString("%1 ")
                       .arg(static_cast<uint>(data[i + j]), 2, 16, QChar('0'))
                       .toUpper();
        }
        out += "\n";
    }
    return out;
}

std::vector<unsigned char> MainWindow::ParseHexDump(const QString& text) {
    std::vector<unsigned char> bytes;
    const QStringList lines = text.split(QRegularExpression("[\r\n]"));
    for (const QString& line : lines) {
        QString t = line.trimmed();
        if (t.isEmpty()) continue;
        int colon = t.indexOf(':');
        if (colon >= 0) t = t.mid(colon + 1).trimmed();
        const QStringList tokens = t.split(QRegularExpression("[ \t]"), Qt::SkipEmptyParts);
        for (const QString& tok : tokens) {
            if (tok.length() == 2) {
                bool ok = false;
                unsigned char b = static_cast<unsigned char>(tok.toUInt(&ok, 16));
                if (ok) bytes.push_back(b);
            }
        }
    }
    return bytes;
}

// ============================================================================
// Config save
// ============================================================================

void MainWindow::OnSaveConfig() {
    try {
        cfg_.anchor_address = cfg_boxes_["AnchorAddress"]->text().toStdString();
        int v = 0;
        if (!TryParseInt(cfg_boxes_["ItemBaseAdjust"]->text().toStdString(), v))
            throw std::runtime_error("ItemBaseAdjust 不是整数");
        cfg_.item_base_adjust = v;
        cfg_.process_access = cfg_boxes_["ProcessAccess"]->text().toStdString();

        auto read_int = [&](const std::string& key, const char* disp) -> int {
            int x = 0;
            if (!TryParseInt(cfg_boxes_[key]->text().toStdString(), x))
                throw std::runtime_error(std::string(disp) + " 不是整数");
            return x;
        };
        cfg_.property_list.base_offset_from_item = read_int("BaseOffsetFromItem", "BaseOffsetFromItem");
        cfg_.property_list.count = read_int("Count", "Count");
        cfg_.property_list.stride = read_int("Stride", "Stride");
        cfg_.property_list.index_offset = read_int("IndexOffset", "IndexOffset");
        cfg_.property_list.num_offset = read_int("NumOffset", "NumOffset");
        cfg_.property_list.color_offset = read_int("ColorOffset", "ColorOffset");
        cfg_.property_list.precent_offset = read_int("PrecentOffset", "PrecentOffset");

        bool b = false;
        if (!TryParseBool(cfg_boxes_["ScanArray"]->text().toStdString(), b))
            throw std::runtime_error("ScanArray 不是 true/false");
        cfg_.property_list.scan_array = b;

        cfg_.Save(CfgPath());
        ReloadConfig();
        Status(QString::fromUtf8("配置已保存并应用。"));
    } catch (const std::exception& ex) {
        Status(QString::fromUtf8("保存配置失败：") + QString::fromUtf8(ex.what()));
    }
}

void MainWindow::Status(const QString& msg) {
    status_text_->setText(msg);
}

}  // namespace poq
