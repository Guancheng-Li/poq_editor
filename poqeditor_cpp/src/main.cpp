#include <QApplication>

#include "MainWindow.h"

int main(int argc, char* argv[]) {
    // Qt6 uses UTF-8 as the default text codec, so source-string literals and
    // fromUtf8/toUtf8 I/O are UTF-8 out of the box (no QTextCodec setup needed).
    QApplication app(argc, argv);
    app.setApplicationName(QString::fromUtf8("PoqEditorCpp"));

    poq::MainWindow window;
    window.show();
    return app.exec();
}
