# Repository guidance

## Build

The project targets Windows with MSVC, CMake 3.16 or newer, and Dear ImGui. Dear
ImGui is intentionally external to this repository. Configure CMake with
`-DIMGUI_DIR=<path-to-imgui>`; that directory must contain `imgui.h` and the
`backends` directory.

Do not commit generated build trees, executables, object files, IDE settings,
local test corpora, or machine-specific paths. See `.gitignore` for the current
policy.

## Code conventions

Keep source files UTF-8. The CMake configuration enables strict MSVC warnings;
changes must compile cleanly with `/W4 /WX`.

Format C++ source and headers with the repository's `.clang-format` before
committing. Use the project's Clang tooling configuration in `.clang-tidy` to
run static analysis on changed C++ files, and resolve reported issues rather
than suppressing checks without a documented reason. Keep include ordering and
Win32 header guards consistent with the existing source.
