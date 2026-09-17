#include "ymt/memory_access.hpp"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

struct ExpectedProtection {
    std::uint32_t protection;
    bool read;
    bool write;
    bool execute;
};

// Explicit expected rights from the Windows contract, with the additional
// readable-code requirement for Execute. These values do not call permits().
constexpr std::array<ExpectedProtection, 8> expectedProtections{{
    {0x01, false, false, false}, // PAGE_NOACCESS
    {0x02, true,  false, false}, // PAGE_READONLY
    {0x04, true,  true,  false}, // PAGE_READWRITE
    {0x08, true,  true,  false}, // PAGE_WRITECOPY
    {0x10, false, false, false}, // PAGE_EXECUTE cannot be inspected as code
    {0x20, true,  false, true }, // PAGE_EXECUTE_READ
    {0x40, true,  true,  true }, // PAGE_EXECUTE_READWRITE
    {0x80, true,  true,  true }, // PAGE_EXECUTE_WRITECOPY
}};

void check_rights(std::uint32_t protection, bool read, bool write, bool execute) {
    const int failuresBefore = failures;
    check(ymt::permits(protection, ymt::Access::Read) == read,
          "read permission matches expected policy");
    check(ymt::permits(protection, ymt::Access::Write) == write,
          "write permission matches expected policy");
    check(ymt::permits(protection, ymt::Access::Execute) == execute,
          "readable executable permission matches expected policy");
    if (failures != failuresBefore) {
        std::cerr << "  protection=0x" << std::hex << protection << std::dec << '\n';
    }
}

void test_full_base_protection_matrix() {
    // Exercise every low-byte value, including zero, unknown values, and
    // invalid combinations such as NOACCESS | READWRITE or READONLY | READWRITE.
    for (std::uint32_t base = 0; base <= 0xff; ++base) {
        ExpectedProtection expected{base, false, false, false};
        for (const auto& entry : expectedProtections) {
            if (entry.protection == base) {
                expected = entry;
                break;
            }
        }
        for (const std::uint32_t modifier : {0U, 0x200U, 0x400U}) {
            check_rights(base | modifier, expected.read, expected.write,
                         expected.execute);
            check_rights(base | modifier | 0x100U, false, false, false);
        }
        check(!ymt::permits(base, static_cast<ymt::Access>(99)),
              "unknown access request is rejected");
    }
}

void test_runtime_regressions() {
    check(ymt::permits(0x40, ymt::Access::Write),
          "RWX game globals remain writable after another ASI changes protection");
    check(ymt::permits(0x04, ymt::Access::Read),
          "a writable vtable is still readable");
    check(ymt::permits(0x20, ymt::Access::Read),
          "a readable executable vtable is still readable");
    check(ymt::permits(0x40, ymt::Access::Read),
          "an RWX vtable is still readable");
    check(!ymt::permits(0x02, ymt::Access::Write),
          "a readable global cannot be treated as writable");
    check(!ymt::permits(0x20, ymt::Access::Write),
          "readable executable code cannot be treated as writable data");
    check(!ymt::permits(0x10, ymt::Access::Execute),
          "execute-only pages cannot safely supply instruction bytes");
    check_rights(0x101, false, false, false); // GUARD | NOACCESS
    check_rights(0x141, false, false, false); // GUARD | NOACCESS | RWX
    check_rights(0x41, false, false, false);  // NOACCESS | RWX is invalid
    check_rights(0x100, false, false, false); // GUARD with no base
    check_rights(0x200, false, false, false); // NOCACHE with no base
    check_rights(0x400, false, false, false); // WRITECOMBINE with no base
    check_rights(0x240, true, true, true);   // NOCACHE does not remove RWX
    check_rights(0x480, true, true, true);   // WRITECOMBINE keeps copy-on-write
}

static_assert(ymt::permits(0x40, ymt::Access::Write), "policy supports constexpr");
static_assert(!ymt::permits(0x140, ymt::Access::Write), "guard blocks writes");

} // namespace

int main() {
    test_full_base_protection_matrix();
    test_runtime_regressions();
    if (failures != 0) {
        std::cerr << failures << " of " << checks << " memory access checks failed\n";
        return 1;
    }
    std::cout << checks << " memory access checks passed\n";
    return 0;
}
