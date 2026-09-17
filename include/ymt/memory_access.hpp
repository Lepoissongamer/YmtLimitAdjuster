#pragma once

#include <cstdint>

namespace ymt {

enum class Access { Read, Write, Execute };

// Windows PAGE_* values are part of the WinNT.h ABI. Keep the policy portable
// so the same checks used by the plugin can be tested without Windows headers.
// https://learn.microsoft.com/en-us/windows/win32/memory/memory-protection-constants
// Execute means readable executable code: our callers inspect instruction bytes
// before invoking or hooking a function, so PAGE_EXECUTE alone is insufficient.
constexpr bool permits(std::uint32_t protection, Access access) {
    constexpr std::uint32_t guard = 0x100;
    if ((protection & guard) != 0) {
        return false;
    }

    // PAGE_NOCACHE and PAGE_WRITECOMBINE modify caching, not access rights.
    // Base protections are alternatives, not independent capability bits.
    switch (protection & 0xff) {
    case 0x02: // PAGE_READONLY
        return access == Access::Read;
    case 0x04: // PAGE_READWRITE
    case 0x08: // PAGE_WRITECOPY
        return access == Access::Read || access == Access::Write;
    case 0x20: // PAGE_EXECUTE_READ
        return access == Access::Read || access == Access::Execute;
    case 0x40: // PAGE_EXECUTE_READWRITE
    case 0x80: // PAGE_EXECUTE_WRITECOPY
        return access == Access::Read || access == Access::Write ||
               access == Access::Execute;
    default: // PAGE_NOACCESS, PAGE_EXECUTE, absent or invalid base protection
        return false;
    }
}

} // namespace ymt
