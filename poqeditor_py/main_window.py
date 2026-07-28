# -*- coding: utf-8 -*-
"""主窗口 UI 与逻辑。对应 C# 的 MainWindow.xaml + MainWindow.xaml.cs。

用 PySide6 程序化构建 UI（无 .ui 文件），布局与交互逻辑镜像 C# 参考。
"""

from __future__ import annotations

import os
from typing import Dict, List, Optional

from PySide6.QtCore import Qt, QPoint, QRect, QSize
from PySide6.QtGui import QFont
from PySide6.QtWidgets import (
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QTableWidget,
    QTableWidgetItem,
    QTabWidget,
    QVBoxLayout,
    QWidget,
    QLayout,
    QHeaderView,
)

import ini_template
from game_modder import GameModder
from models import ItemProperty
from offsets_config import OffsetsConfig
from process_memory import ProcessInfo, ProcessMemory


APP_DIR = os.path.dirname(os.path.abspath(__file__))
CFG_PATH = os.path.join(APP_DIR, "offsets.json")


class FlowLayout(QLayout):
    """流式布局，类似 WPF 的 WrapPanel：子部件按行排列，超出宽度自动换行。"""

    def __init__(self, parent: Optional[QWidget] = None, margin: int = 0, spacing: int = 6) -> None:
        super().__init__(parent)
        if parent is not None:
            self.setContentsMargins(margin, margin, margin, margin)
        self.setSpacing(spacing)
        self._item_list: List = []

    def __del__(self) -> None:
        while self.count():
            self.takeAt(0)

    def addItem(self, item) -> None:  # type: ignore[override]
        self._item_list.append(item)

    def count(self) -> int:  # type: ignore[override]
        return len(self._item_list)

    def itemAt(self, index: int):  # type: ignore[override]
        if 0 <= index < len(self._item_list):
            return self._item_list[index]
        return None

    def takeAt(self, index: int):  # type: ignore[override]
        if 0 <= index < len(self._item_list):
            return self._item_list.pop(index)
        return None

    def expandingDirections(self) -> Qt.Orientation:  # type: ignore[override]
        return Qt.Orientation(0)

    def hasHeightForWidth(self) -> bool:  # type: ignore[override]
        return True

    def heightForWidth(self, width: int) -> int:  # type: ignore[override]
        return self.doLayout(QRect(0, 0, width, 0), test_only=True)

    def setGeometry(self, rect: QRect) -> None:  # type: ignore[override]
        super().setGeometry(rect)
        self.doLayout(rect, test_only=False)

    def sizeHint(self) -> QSize:  # type: ignore[override]
        return self.minimumSize()

    def minimumSize(self) -> QSize:  # type: ignore[override]
        size = QSize()
        for item in self._item_list:
            size = size.expandedTo(item.minimumSize())
        m = self.contentsMargins()
        size += QSize(m.left() + m.right(), m.top() + m.bottom())
        return size

    def doLayout(self, rect: QRect, test_only: bool) -> int:
        x = rect.x()
        y = rect.y()
        line_height = 0
        spacing = self.spacing()
        for item in self._item_list:
            wid = item.widget()
            space_x = spacing
            space_y = spacing
            if wid is not None:
                space_x = spacing + wid.style().layoutSpacing(
                    QSizePolicy.ControlType.PushButton,
                    QSizePolicy.ControlType.PushButton,
                    Qt.Orientation.Horizontal,
                )
                space_y = spacing + wid.style().layoutSpacing(
                    QSizePolicy.ControlType.PushButton,
                    QSizePolicy.ControlType.PushButton,
                    Qt.Orientation.Vertical,
                )
            next_x = x + item.sizeHint().width() + space_x
            if next_x - space_x > rect.right() and line_height > 0:
                x = rect.x()
                y = y + line_height + space_y
                next_x = x + item.sizeHint().width() + space_x
                line_height = 0
            if not test_only:
                item.setGeometry(QRect(QPoint(x, y), item.sizeHint()))
            x = next_x
            line_height = max(line_height, item.sizeHint().height())
        return y + line_height - rect.y()


class MainWindow(QMainWindow):
    """主窗口。"""

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("秦殇内存修改器 (PoqEditor) - 64位重制版")
        self.resize(1100, 720)

        self._cfg: OffsetsConfig = OffsetsConfig.default()
        self._modder: GameModder = GameModder(self._cfg)
        self._basic_boxes: Dict[str, QLineEdit] = {}
        self._cfg_boxes: Dict[str, QLineEdit] = {}
        self._processes: List[ProcessInfo] = []

        self._build_ui()
        self._reload_config()
        self._refresh_processes()
        self.status("就绪。请以管理员身份运行，并在游戏中拾起要修改的物品。")

    # ------------------------------------------------------------------
    # UI 构建
    # ------------------------------------------------------------------
    def _build_ui(self) -> None:
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(6, 6, 6, 6)
        root.setSpacing(6)

        # ---- 顶部：进程选择 ----
        proc_group = QGroupBox("进程")
        proc_h = QHBoxLayout(proc_group)
        proc_h.addWidget(QLabel("选择进程："))
        self.cb_process = QComboBox()
        self.cb_process.setEditable(True)
        self.cb_process.setMinimumWidth(320)
        proc_h.addWidget(self.cb_process, 1)
        self.btn_refresh = QPushButton("刷新进程")
        self.btn_refresh.clicked.connect(self._on_refresh)
        proc_h.addWidget(self.btn_refresh)
        self.btn_init = QPushButton("初始化")
        f = self.btn_init.font()
        f.setBold(True)
        self.btn_init.setFont(f)
        self.btn_init.clicked.connect(self._on_init)
        proc_h.addWidget(self.btn_init)
        root.addWidget(proc_group)

        # ---- TabWidget ----
        self.tabs = QTabWidget()
        self.tabs.addTab(self._build_item_tab(), "物品修改")
        self.tabs.addTab(self._build_mem_tab(), "内存查看")
        self.tabs.addTab(self._build_cfg_tab(), "偏移配置")
        root.addWidget(self.tabs, 1)

        # 状态栏（QMainWindow 自带）
        self._status_label = QLabel()
        self.statusBar().addWidget(self._status_label, 1)

    # ---- 物品修改 Tab ----
    def _build_item_tab(self) -> QWidget:
        tab = QWidget()
        v = QVBoxLayout(tab)
        v.setContentsMargins(4, 4, 4, 4)
        v.setSpacing(6)

        # 基础属性
        self.basic_group = QGroupBox("基础属性")
        self.basic_container = QWidget()
        self.basic_layout = FlowLayout(self.basic_container, margin=6, spacing=6)
        self.basic_group_layout = QVBoxLayout(self.basic_group)
        self.basic_group_layout.addWidget(self.basic_container)
        v.addWidget(self.basic_group)

        # 属性列表
        self.prop_group = QGroupBox("属性列表（46 条模板）")
        gv = QVBoxLayout(self.prop_group)
        btn_row = QHBoxLayout()
        self.btn_read = QPushButton("读取属性")
        self.btn_read.setFont(self._bold_font())
        self.btn_read.clicked.connect(self._on_read)
        btn_row.addWidget(self.btn_read)
        self.btn_load_tpl = QPushButton("从模板读取")
        self.btn_load_tpl.clicked.connect(self._on_load_tpl)
        btn_row.addWidget(self.btn_load_tpl)
        self.btn_save = QPushButton("保存属性")
        self.btn_save.setFont(self._bold_font())
        self.btn_save.clicked.connect(self._on_save)
        btn_row.addWidget(self.btn_save)
        self.btn_add = QPushButton("添加")
        self.btn_add.clicked.connect(self._on_add)
        btn_row.addWidget(self.btn_add)
        self.btn_del = QPushButton("删除")
        self.btn_del.clicked.connect(self._on_del)
        btn_row.addWidget(self.btn_del)
        self.btn_clear = QPushButton("清空")
        self.btn_clear.clicked.connect(self._on_clear)
        btn_row.addWidget(self.btn_clear)
        self.lb_count = QLabel("属性数：0")
        self.lb_count.setContentsMargins(12, 0, 0, 0)
        btn_row.addWidget(self.lb_count)
        btn_row.addStretch(1)
        gv.addLayout(btn_row)

        self.prop_grid = QTableWidget(0, 5)
        self.prop_grid.setHorizontalHeaderLabels([
            "地址(写入位置)", "Index(属性代码)", "Num(数值)",
            "Color(颜色)", "Precent(百分比)",
        ])
        header = self.prop_grid.horizontalHeader()
        header.setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
        self.prop_grid.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        gv.addWidget(self.prop_grid, 1)

        v.addWidget(self.prop_group, 1)
        return tab

    # ---- 内存查看 Tab ----
    def _build_mem_tab(self) -> QWidget:
        tab = QWidget()
        v = QVBoxLayout(tab)
        v.setContentsMargins(6, 6, 6, 6)
        v.setSpacing(6)

        row = QHBoxLayout()
        row.addWidget(QLabel("地址(0x...)"))
        self.raw_addr = QLineEdit("0x901FF0")
        self.raw_addr.setMaximumWidth(160)
        row.addWidget(self.raw_addr)
        row.addSpacing(8)
        row.addWidget(QLabel("长度"))
        self.raw_size = QLineEdit("64")
        self.raw_size.setMaximumWidth(60)
        row.addWidget(self.raw_size)
        self.btn_raw_read = QPushButton("读取")
        self.btn_raw_read.clicked.connect(self._on_raw_read)
        row.addWidget(self.btn_raw_read)
        self.btn_raw_write = QPushButton("写入")
        self.btn_raw_write.clicked.connect(self._on_raw_write)
        row.addWidget(self.btn_raw_write)
        row.addStretch(1)
        v.addLayout(row)

        self.raw_hex = QPlainTextEdit()
        mono = QFont("Consolas")
        mono.setStyleHint(QFont.StyleHint.Monospace)
        self.raw_hex.setFont(mono)
        self.raw_hex.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        v.addWidget(self.raw_hex, 1)

        note = QLabel(
            "写入时：上面 hex 文本框里每行 \"地址: 十六进制字节\" 或纯十六进制字节均可，"
            "程序会解析后写入。读取用于校准偏移。"
        )
        note.setStyleSheet("color: gray;")
        note.setWordWrap(True)
        v.addWidget(note)
        return tab

    # ---- 偏移配置 Tab ----
    def _build_cfg_tab(self) -> QWidget:
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        inner = QWidget()
        v = QVBoxLayout(inner)
        v.setContentsMargins(6, 6, 6, 6)
        v.setSpacing(4)
        self._cfg_layout = v
        scroll.setWidget(inner)
        return scroll

    @staticmethod
    def _bold_font() -> QFont:
        f = QFont()
        f.setBold(True)
        return f

    # ------------------------------------------------------------------
    # 配置
    # ------------------------------------------------------------------
    def _reload_config(self) -> None:
        self._cfg = OffsetsConfig.load(CFG_PATH)
        self._modder = GameModder(self._cfg)
        self._build_basic_fields_panel()
        self._build_config_panel()

    def _build_basic_fields_panel(self) -> None:
        # 清空旧部件
        while self.basic_layout.count():
            item = self.basic_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()
        self._basic_boxes.clear()
        for f in self._cfg.basic_fields:
            row = QWidget()
            row.setStyleSheet(
                "QWidget { border: 1px solid lightgray; border-radius: 2px; }"
            )
            rh = QHBoxLayout(row)
            rh.setContentsMargins(6, 3, 6, 3)
            lbl = QLabel(f"{f.label} (+0x{f.offset:X})")
            tb = QLineEdit("0")
            tb.setMaximumWidth(90)
            tb.setMinimumWidth(60)
            rh.addWidget(lbl)
            rh.addWidget(tb)
            self.basic_layout.addWidget(row)
            self._basic_boxes[f.label] = tb

    def _build_config_panel(self) -> None:
        # 清空
        while self._cfg_layout.count():
            item = self._cfg_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()
        self._cfg_boxes.clear()

        def add_row(label: str, value: str, key: str) -> None:
            row = QHBoxLayout()
            row.setContentsMargins(2, 2, 2, 2)
            lbl = QLabel(label)
            lbl.setMinimumWidth(260)
            lbl.setMaximumWidth(260)
            tb = QLineEdit(value)
            tb.setMaximumWidth(240)
            row.addWidget(lbl)
            row.addWidget(tb)
            row.addStretch(1)
            container = QWidget()
            container.setLayout(row)
            self._cfg_layout.addWidget(container)
            self._cfg_boxes[key] = tb

        pl = self._cfg.property_list
        add_row("锚点地址 (AnchorAddress)", self._cfg.anchor_address, "AnchorAddress")
        add_row("物品基址调整 (ItemBaseAdjust, -0xF0=-240)", str(self._cfg.item_base_adjust), "ItemBaseAdjust")
        add_row("进程访问权限 (ProcessAccess)", self._cfg.process_access, "ProcessAccess")
        add_row("属性数组起点 (BaseOffsetFromItem)", str(pl.base_offset_from_item), "BaseOffsetFromItem")
        add_row("属性数量 (Count)", str(pl.count), "Count")
        add_row("属性槽步长 (Stride)", str(pl.stride), "Stride")
        add_row("Index 偏移", str(pl.index_offset), "IndexOffset")
        add_row("Num 偏移", str(pl.num_offset), "NumOffset")
        add_row("Color 偏移", str(pl.color_offset), "ColorOffset")
        add_row("Precent 偏移", str(pl.precent_offset), "PrecentOffset")
        add_row("读取时扫描数组 (ScanArray, true/false)", str(pl.scan_array).lower(), "ScanArray")

        self.btn_save_cfg = QPushButton("保存配置并应用")
        self.btn_save_cfg.setContentsMargins(2, 12, 2, 2)
        self.btn_save_cfg.clicked.connect(self._on_save_cfg)
        self._cfg_layout.addWidget(self.btn_save_cfg)

        note = QLabel(
            "说明：以上偏移来自对原版 poq_hack.exe 的逆向，默认值可能需要针对实际 POQ.EXE v1.30 校准。\n"
            "可用\"内存查看\"页读取物品内存来核对。修改后点击\"保存配置并应用\"即写入 offsets.json 并即时生效。"
        )
        note.setStyleSheet("color: gray;")
        note.setWordWrap(True)
        self._cfg_layout.addWidget(note)
        self._cfg_layout.addStretch(1)

    # ------------------------------------------------------------------
    # 进程
    # ------------------------------------------------------------------
    def _on_refresh(self) -> None:
        self._refresh_processes()

    def _refresh_processes(self) -> None:
        self._processes = ProcessMemory.enumerate_processes()
        self.cb_process.clear()
        poq: Optional[ProcessInfo] = None
        for p in self._processes:
            self.cb_process.addItem(str(p), userData=p)
            if p.name.lower() == "poq.exe":
                poq = p
        if poq is not None:
            i = self._processes.index(poq)
            self.cb_process.setCurrentIndex(i)
        msg = f"已列出 {len(self._processes)} 个进程。"
        if poq is not None:
            msg += "已自动选中 POQ.EXE。"
        else:
            msg += "未找到 POQ.EXE，请手动选择或启动游戏。"
        self.status(msg)

    def _try_get_selected_pid(self) -> Optional[int]:
        # WPF 可编辑 ComboBox：选中下拉项时 SelectedItem 为该对象；用户手输非匹配文本
        # 时 SelectedItem 为 null。这里用“当前文本是否等于某进程显示串”等价判定。
        txt = self.cb_process.currentText().strip()
        # 1. 当前文本恰好等于某个进程的显示串（含选中项）
        for p in self._processes:
            if txt == str(p):
                return p.pid
        # 2. 用户可能手输入了数字 PID
        try:
            return int(txt)
        except ValueError:
            pass
        # 3. 按名字匹配
        for p in self._processes:
            if p.name.lower() == txt.lower():
                return p.pid
        return None

    def _on_init(self) -> None:
        pid = self._try_get_selected_pid()
        if pid is None:
            self.status("请先选择一个进程。")
            return
        try:
            ptr = self._modder.initialize(pid)
            self.status(
                f"初始化成功。手持指针 = 0x{ptr:08X}"
                f"（物品基址 = 0x{self._modder.get_item_base():08X}）。"
            )
        except Exception as ex:
            self.status("初始化失败：" + str(ex))

    # ------------------------------------------------------------------
    # 属性按钮
    # ------------------------------------------------------------------
    def _on_read(self) -> None:
        if not self._modder.mem.is_open:
            self.status("请先点击\"初始化\"。")
            return
        try:
            ib = self._modder.get_item_base()
            basic = self._modder.read_basic_fields(ib)
            for k, v in basic.items():
                tb = self._basic_boxes.get(k)
                if tb is not None:
                    tb.setText(str(v))
            props = self._modder.read_properties(ib)
            self._write_table(props)
            self._update_count()
            self.status(f"读取完成。物品基址 = 0x{ib:08X}，属性 {len(props)} 条。")
        except Exception as ex:
            self.status("读取失败：" + str(ex))

    def _on_load_tpl(self) -> None:
        path = os.path.join(APP_DIR, self._cfg.template_ini)
        try:
            tpl = ini_template.load(path)
            if not tpl:
                self.status("模板为空或文件不存在：" + path)
                return
            current = self._read_table()
            has_addr = any(p.address.strip() for p in current)
            if current and has_addr:
                # 按 Index 匹配模板数值到现有行
                for row in current:
                    m = next((t for t in tpl if t.index == row.index), None)
                    if m is not None:
                        row.num = m.num
                        row.color = m.color
                        row.precent = m.precent
                self._write_table(current)
                self.status(f"已按 Index 匹配模板数值到现有 {len(current)} 条属性。")
            else:
                self._write_table(tpl)
                self.status(
                    f"已从模板载入 {len(tpl)} 条属性（地址列为空，保存前请先\"读取属性\"或手填地址）。"
                )
            self._update_count()
        except Exception as ex:
            self.status("载入模板失败：" + str(ex))

    def _on_save(self) -> None:
        if not self._modder.mem.is_open:
            self.status("请先点击\"初始化\"。")
            return
        props = self._read_table()
        if not props or not any(p.address.strip() for p in props):
            self.status("没有可写入的属性地址。请先\"读取属性\"或在地址列填入地址。")
            return
        try:
            ib = self._modder.get_item_base()
            # 基础属性
            basic: Dict[str, int] = {}
            for k, tb in self._basic_boxes.items():
                try:
                    v = int(tb.text())
                    basic[k] = v
                except ValueError:
                    pass
            self._modder.write_basic_fields(ib, basic)
            # 属性列表
            self._modder.write_properties(props)
            written = sum(1 for p in props if p.address.strip())
            self.status(
                f"保存完成：基础属性 {len(basic)} 项，属性 {written} 条已写入。"
                "请在游戏中把物品扔到地上再捡起使修改生效。"
            )
        except Exception as ex:
            self.status("保存失败：" + str(ex))

    def _on_add(self) -> None:
        self._append_row(ItemProperty())
        self._update_count()

    def _on_del(self) -> None:
        rows = sorted({idx.row() for idx in self.prop_grid.selectedIndexes()}, reverse=True)
        if not rows:
            self.status("请先在表格里选中要删除的行。")
            return
        for r in rows:
            self.prop_grid.removeRow(r)
        self._update_count()

    def _on_clear(self) -> None:
        self.prop_grid.setRowCount(0)
        self._update_count()

    def _update_count(self) -> None:
        self.lb_count.setText(f"属性数：{self.prop_grid.rowCount()}")

    # ---- 表格工具 ----
    def _read_table(self) -> List[ItemProperty]:
        props: List[ItemProperty] = []
        for r in range(self.prop_grid.rowCount()):
            props.append(ItemProperty(
                address=self._cell_text(r, 0),
                index=self._cell_int(r, 1),
                num=self._cell_int(r, 2),
                color=self._cell_int(r, 3),
                precent=self._cell_int(r, 4),
            ))
        return props

    def _write_table(self, props: List[ItemProperty]) -> None:
        self.prop_grid.setRowCount(0)
        for p in props:
            self._append_row(p)

    def _append_row(self, p: ItemProperty) -> None:
        r = self.prop_grid.rowCount()
        self.prop_grid.insertRow(r)
        self._set_cell(r, 0, p.address)
        self._set_cell(r, 1, str(p.index))
        self._set_cell(r, 2, str(p.num))
        self._set_cell(r, 3, str(p.color))
        self._set_cell(r, 4, str(p.precent))

    def _set_cell(self, row: int, col: int, text: str) -> None:
        item = QTableWidgetItem(text)
        self.prop_grid.setItem(row, col, item)

    def _cell_text(self, row: int, col: int) -> str:
        item = self.prop_grid.item(row, col)
        return item.text() if item is not None else ""

    def _cell_int(self, row: int, col: int) -> int:
        item = self.prop_grid.item(row, col)
        if item is None:
            return 0
        try:
            return int(item.text())
        except ValueError:
            return 0

    # ------------------------------------------------------------------
    # 内存查看
    # ------------------------------------------------------------------
    def _on_raw_read(self) -> None:
        if not self._modder.mem.is_open:
            self.status("请先\"初始化\"。")
            return
        try:
            addr = ProcessMemory.parse_address(self.raw_addr.text())
            try:
                size = int(self.raw_size.text())
            except ValueError:
                size = 64
            data = self._modder.mem.read(addr, size)
            self.raw_hex.setPlainText(self._format_hex_dump(addr, data))
            self.status(f"已读取 0x{addr:08X} 共 {len(data)} 字节。")
        except Exception as ex:
            self.status("读取失败：" + str(ex))

    def _on_raw_write(self) -> None:
        if not self._modder.mem.is_open:
            self.status("请先\"初始化\"。")
            return
        try:
            addr = ProcessMemory.parse_address(self.raw_addr.text())
            data = self._parse_hex_dump(self.raw_hex.toPlainText())
            if not data:
                self.status("没有可写入的十六进制字节。")
                return
            self._modder.mem.write(addr, bytes(data))
            self.status(f"已写入 0x{addr:08X} 共 {len(data)} 字节。")
        except Exception as ex:
            self.status("写入失败：" + str(ex))

    @staticmethod
    def _format_hex_dump(base_addr: int, data: bytes) -> str:
        lines: List[str] = []
        n = len(data)
        i = 0
        while i < n:
            line = f"{(base_addr + i) & 0xFFFFFFFF:08X}: "
            chunk = data[i:i + 16]
            line += " ".join(f"{b:02X}" for b in chunk)
            lines.append(line)
            i += 16
        return "\n".join(lines)

    @staticmethod
    def _parse_hex_dump(text: str) -> List[int]:
        result: List[int] = []
        for line in text.splitlines():
            t = line.strip()
            if not t:
                continue
            colon = t.find(":")
            if colon >= 0:
                t = t[colon + 1:].strip()
            for tok in t.replace("\t", " ").split(" "):
                if len(tok) == 2:
                    try:
                        result.append(int(tok, 16))
                    except ValueError:
                        pass
        return result

    # ------------------------------------------------------------------
    # 偏移配置保存
    # ------------------------------------------------------------------
    def _on_save_cfg(self) -> None:
        try:
            self._cfg.anchor_address = self._cfg_boxes["AnchorAddress"].text()
            self._cfg.item_base_adjust = int(self._cfg_boxes["ItemBaseAdjust"].text())
            self._cfg.process_access = self._cfg_boxes["ProcessAccess"].text()
            pl = self._cfg.property_list
            pl.base_offset_from_item = int(self._cfg_boxes["BaseOffsetFromItem"].text())
            pl.count = int(self._cfg_boxes["Count"].text())
            pl.stride = int(self._cfg_boxes["Stride"].text())
            pl.index_offset = int(self._cfg_boxes["IndexOffset"].text())
            pl.num_offset = int(self._cfg_boxes["NumOffset"].text())
            pl.color_offset = int(self._cfg_boxes["ColorOffset"].text())
            pl.precent_offset = int(self._cfg_boxes["PrecentOffset"].text())
            sa_text = self._cfg_boxes["ScanArray"].text().strip().lower()
            # 对应 C# bool.Parse：仅接受 true/false（大小写不敏感），否则抛异常
            if sa_text == "true":
                pl.scan_array = True
            elif sa_text == "false":
                pl.scan_array = False
            else:
                raise ValueError("ScanArray 必须为 true 或 false")
            self._cfg.save(CFG_PATH)
            self._reload_config()
            self.status("配置已保存并应用。")
        except Exception as ex:
            self.status("保存配置失败：" + str(ex))

    # ------------------------------------------------------------------
    # 状态栏
    # ------------------------------------------------------------------
    def status(self, msg: str) -> None:
        self._status_label.setText(msg)
