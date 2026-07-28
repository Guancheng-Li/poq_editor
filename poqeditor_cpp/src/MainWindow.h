#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include <QMainWindow>
#include <QString>

#include "Core/GameModder.h"
#include "Core/ItemProperty.h"
#include "Native/ProcessMemory.h"

class QComboBox;
class QPushButton;
class QLabel;
class QTabWidget;
class QGroupBox;
class QWidget;
class QTableWidget;
class QLineEdit;
class QPlainTextEdit;
class QScrollArea;

namespace poq {

/// Main application window. Builds its UI programmatically (no .ui file) to
/// match the C# code-behind style. Mirrors `PoqEditor.MainWindow`.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void OnRefreshProcesses();
    void OnInit();
    void OnReadProperties();
    void OnLoadTemplate();
    void OnSaveProperties();
    void OnAddProperty();
    void OnDeleteProperty();
    void OnClearProperties();
    void OnRawRead();
    void OnRawWrite();
    void OnSaveConfig();

private:
    void ReloadConfig();
    void RefreshProcesses();
    bool TryGetSelectedPid(unsigned int& pid);
    void BuildBasicFieldsPanel();
    void BuildConfigPanel();
    void UpdateCount();
    void Status(const QString& msg);

    // Table<->props helpers. The QTableWidget is the persistent store; these
    // collect its current contents into ItemProperty rows for writing.
    std::vector<ItemProperty> CollectPropsFromTable();
    bool AnyRowHasAddress();

    // Hex dump helpers, matching the C# FormatHexDump / ParseHexDump.
    static QString FormatHexDump(uintptr_t base_addr, const std::vector<unsigned char>& data);
    static std::vector<unsigned char> ParseHexDump(const QString& text);

    // Returns the application directory (where offsets.json / EquMod.ini live).
    static QString AppDir();
    static QString CfgPath();

    // --- config / core ---
    OffsetsConfig cfg_;
    std::unique_ptr<GameModder> modder_;
    std::vector<ProcessInfo> processes_;
    // Maps a basic-field label -> its line edit.
    std::unordered_map<std::string, QLineEdit*> basic_boxes_;
    // Maps a config key -> its line edit.
    std::unordered_map<std::string, QLineEdit*> cfg_boxes_;

    // --- top bar widgets ---
    QComboBox* cb_process_ = nullptr;
    QPushButton* btn_refresh_ = nullptr;
    QPushButton* btn_init_ = nullptr;
    QLabel* status_text_ = nullptr;

    // --- item tab widgets ---
    QWidget* basic_fields_panel_ = nullptr;
    QTableWidget* prop_grid_ = nullptr;
    QLabel* lb_count_ = nullptr;

    // --- memory tab widgets ---
    QLineEdit* raw_addr_ = nullptr;
    QLineEdit* raw_size_ = nullptr;
    QPlainTextEdit* raw_hex_ = nullptr;

    // --- config tab widgets ---
    QWidget* cfg_panel_ = nullptr;
};

}  // namespace poq
