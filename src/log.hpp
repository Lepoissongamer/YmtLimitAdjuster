#pragma once
#include <windows.h>
#include <string>

namespace ymt {
bool open_log(const std::wstring& path);
void log(const char* format, ...);
[[noreturn]] void fail_stop(const char* message);
}
