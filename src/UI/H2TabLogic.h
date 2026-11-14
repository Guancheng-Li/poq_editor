#pragma once

#include <string>

namespace poq {

struct App;

std::wstring h2_backup_path(const std::wstring& path, bool decrypt);
void h2_do_convert(App& app, const std::wstring& path, bool decrypt, bool make_backup);
void h2_on_convert(App& app, bool decrypt);
void h2_on_batch(App& app, bool decrypt);
void h2_run_batch(App& app, const std::wstring& dir, bool decrypt);

}  // namespace poq
