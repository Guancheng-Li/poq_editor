# -*- coding: utf-8 -*-
"""秦殇内存修改器 (PoqEditor) 入口。

创建 QApplication 并显示主窗口。仅在 Windows + PySide6 环境下运行。
"""

from __future__ import annotations

import sys

from PySide6.QtWidgets import QApplication

from main_window import MainWindow


def main() -> int:
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
