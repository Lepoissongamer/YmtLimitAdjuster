#include "log.hpp"
#include <cstdarg>
#include <cstdio>

namespace ymt {
namespace {
HANDLE file = INVALID_HANDLE_VALUE;
SRWLOCK lock = SRWLOCK_INIT;
}
bool open_log(const std::wstring& path) {
    file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return file != INVALID_HANDLE_VALUE;
}
void log(const char* format, ...) {
    char message[2048]{};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[2304]{};
    const auto length = std::snprintf(line, sizeof(line),
        "[%02u:%02u:%02u.%03u] %s\r\n", time.wHour, time.wMinute,
        time.wSecond, time.wMilliseconds, message);
    if (length <= 0) return;
    AcquireSRWLockExclusive(&lock);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written{};
        WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
        FlushFileBuffers(file);
    }
    OutputDebugStringA(line);
    ReleaseSRWLockExclusive(&lock);
}
[[noreturn]] void fail_stop(const char* message) {
    log("STOP: %s", message);
    MessageBoxA(nullptr, message, "YmtLimitAdjuster - unsafe dependency state",
                MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    // Continuing would return an incomplete dependency list to the engine.
    // Do not run game destructors on a potentially inconsistent streamer.
    TerminateProcess(GetCurrentProcess(), 0x594D5401u);
    for (;;) Sleep(INFINITE);
}
}
