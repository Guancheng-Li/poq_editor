# PoqEditorCpp

A C++/Qt6 Widgets port of the C# WPF memory modifier ("trainer") for the Chinese
game **秦殇 (Prince of Qin, `POQ.EXE`, 32-bit)**. The original was a VB6 app that
fails on 64-bit Windows; this port rebuilds it as a 64-bit Qt6 application that
reads/writes the 32-bit game process memory through the Win32 API.

This is a faithful port of the C# WPF reference implementation in
`../poqeditor/`. The memory mechanism, offsets, UI layout, and behavior match
that reference exactly.

## Requirements

- **Windows** (the tool calls Win32 process-memory APIs).
- **Qt 6** (Widgets, Core, Gui) — 6.2 or newer.
- **CMake** 3.16+.
- A C++17 compiler: **MSVC** (Visual Studio 2019/2022) or **MinGW** (x64).

## Build

### MSVC (Developer Command Prompt / PowerShell)

```bat
cd poqeditor_cpp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH="C:/Qt/6.7.0/msvc2019_64"
cmake --build build --config Release
```

The executable is `build/Release/PoqEditorCpp.exe`.

### MinGW

```bat
cd poqeditor_cpp
cmake -S . -B build -G "MinGW Makefiles" ^
      -DCMAKE_PREFIX_PATH="C:/Qt/6.7.0/mingw_64"
cmake --build build
```

The executable is `build/PoqEditorCpp.exe`.

### Notes

- `CMAKE_PREFIX_PATH` must point at your Qt installation's MSVC or MinGW folder.
- The CMake build copies `offsets.json` and `EquMod.ini` next to the executable
  via a post-build step, so the tool finds them at runtime relative to the exe.
- The executable embeds an `app.manifest` (via `app.rc`) that requests
  `requireAdministrator`, so launching it triggers a UAC prompt. This is required
  because `OpenProcess(PROCESS_ALL_ACCESS)` on the game needs admin rights.

## Usage

1. Launch the game (POQ.EXE).
2. Run `PoqEditorCpp.exe` (allow elevation).
3. Click **刷新进程**; POQ.EXE is auto-selected if found.
4. Click **初始化** to open the process and read the held-item pointer at the
   fixed address `0x00901FF0`.
5. In the game, **pick up the item** you want to modify (this makes the game
   write the item pointer into `[0x901FF0]`).
6. On the **物品修改** tab, click **读取属性** to read basic fields + the
   46-property array, or **从模板读取** to load `EquMod.ini`.
7. Edit values, then **保存属性** to write them back.
8. In the game, **drop the item and pick it up again** so the change takes
   effect.

The **内存查看** tab reads/writes raw hex bytes at any address — useful for
calibrating offsets. The **偏移配置** tab edits `offsets.json` in place; click
**保存配置并应用** to persist and reload.

## Project layout

```
poqeditor_cpp/
├── CMakeLists.txt          Qt6 build, embeds admin manifest, copies configs
├── app.manifest            requireAdministrator + UTF-8 active code page
├── app.rc                  embeds app.manifest into the exe
├── offsets.json            configurable anchor/offsets (copied to build dir)
├── EquMod.ini              46-property template, UTF-8 (copied to build dir)
├── README.md
└── src/
    ├── main.cpp            QApplication entry point
    ├── MainWindow.h/.cpp   programmatic UI + slots (mirrors MainWindow.xaml.cs)
    ├── Core/
    │   ├── OffsetsConfig.h/.cpp   config model + JSON load/save + defaults
    │   ├── ItemProperty.h         one property row
    │   ├── IniTemplate.h/.cpp     EquMod.ini UTF-8 reader/writer
    │   └── GameModder.h/.cpp      core read/write logic
    └── Native/
        └── ProcessMemory.h/.cpp   Win32 OpenProcess/Read/Write wrapper
```

## Porting notes

- All addresses use `uintptr_t` so a 64-bit editor can poke a 32-bit game.
- `OpenProcess` (the Win32 macro) is called directly inside `ProcessMemory`; the
  wrapper method is named `Open` to avoid the name clash.
- Process enumeration uses the ANSI `PROCESSENTRY32A` variant so `szExeFile` is a
  narrow `char` string regardless of the project's UNICODE setting.
- `EquMod.ini` and `offsets.json` are read/written as UTF-8 (no BOM); the ini
  uses CRLF line endings to match the original VB6 parser.
- Offsets come from reverse-engineering the original `poq_hack.exe` and may need
  calibration against the actual POQ.EXE v1.30 — hence the configurable tab.
