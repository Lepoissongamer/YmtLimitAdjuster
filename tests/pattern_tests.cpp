#include "ymt/pattern.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    int failures{};
    auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    const std::uint8_t bytes[]{0x48, 0x8D, 0x10, 0x00, 0xAB};
    const ymt::Pattern pattern("48 8d ? ?? aB");
    check(pattern.matches(bytes, sizeof(bytes)), "hex and wildcards");
    check(!pattern.matches(bytes, sizeof(bytes) - 1), "truncated buffer");
    check(!pattern.matches(nullptr, 10), "null buffer");
    check(!ymt::Pattern("49 8d ? ?? ab").matches(bytes, sizeof(bytes)), "mismatch");
    check(ymt::Pattern(" \t48\n8d\r? ?? ab  ").matches(bytes, sizeof(bytes)), "whitespace");
    for (const char* bad : {"", " ", "4", "GG", "4?", "???", "0x48"}) {
        bool rejected{};
        try { const ymt::Pattern invalid(bad); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "malformed pattern rejected");
    }
    if (!failures) std::cout << "Pattern tests passed\n";
    return failures ? 1 : 0;
}
