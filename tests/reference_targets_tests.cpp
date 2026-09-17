#include "ymt/reference_targets.hpp"
#include "ymt/pattern.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

template <std::size_t N>
void write_displacement(std::array<std::uint8_t, N>& bytes, std::size_t offset,
                        std::uintptr_t base, std::uintptr_t target) {
    const auto delta = static_cast<std::int64_t>(target) -
                       static_cast<std::int64_t>(base + offset + 4);
    check(delta >= std::numeric_limits<std::int32_t>::min() &&
              delta <= std::numeric_limits<std::int32_t>::max(),
          "synthetic reference fits a signed rel32 field");
    const auto encoded = static_cast<std::uint32_t>(delta);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>(encoded >> (i * 8));
    }
}

std::int32_t read_displacement(const std::uint8_t* bytes) {
    std::uint32_t encoded = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        encoded |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
    }
    // Decode little-endian bytes without host-endian or signed-cast assumptions.
    const std::int64_t signedValue = encoded <= 0x7fffffffU
        ? static_cast<std::int64_t>(encoded)
        : static_cast<std::int64_t>(encoded) - 0x100000000LL;
    return static_cast<std::int32_t>(signedValue);
}

struct ScanResult {
    std::size_t matches = 0;
    ymt::ReferenceTargets targets;
};

template <std::size_t N>
ScanResult scan_references(const std::array<std::uint8_t, N>& bytes,
                           std::uintptr_t base, std::string_view signature,
                           std::size_t displacementOffset) {
    const ymt::Pattern pattern(signature);
    ScanResult result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (!pattern.matches(bytes.data() + i, bytes.size() - i)) {
            continue;
        }
        ++result.matches;
        if (displacementOffset > bytes.size() - i ||
            bytes.size() - i - displacementOffset < 4) {
            continue;
        }
        const auto field = i + displacementOffset;
        const auto target = ymt::relative_target(base + field,
                                                read_displacement(bytes.data() + field));
        if (target) {
            result.targets.add(*target);
        }
    }
    return result;
}

void test_empty_duplicates_and_ambiguity() {
    ymt::ReferenceTargets targets;
    check(targets.distinct_count() == 0 && !targets.unique(),
          "no validated references leaves the symbol unresolved");
    targets.add(0);
    check(targets.distinct_count() == 0 && !targets.unique(),
          "null is never a usable target");
    targets.add(0x1000);
    targets.add(0x1000);
    targets.add(0);
    check(targets.distinct_count() == 1 && targets.unique() == 0x1000,
          "duplicates and null do not create ambiguity");
    targets.add(0x2000);
    check(targets.distinct_count() == 2 && !targets.unique(),
          "two actual destinations refuse resolution");
    for (int i = 0; i < 10; ++i) {
        targets.add(0x1000);
    }
    check(targets.distinct_count() == 2 && !targets.unique(),
          "a majority of duplicate references cannot mask a competing target");
}

void test_streaming_manager_references() {
    // Ten real byte-pattern matches include an LEA RIP-relative disp32 at +11.
    constexpr std::uintptr_t base = 0x10000000;
    constexpr std::uintptr_t managerSlot = 0x11000000;
    constexpr std::array<std::uint8_t, 16> instructionBytes{
        0x74, 0x1a, 0x8b, 0x15, 0, 0, 0, 0,
        0x48, 0x8d, 0x0d, 0, 0, 0, 0, 0x41};
    constexpr auto signature = "74 1A 8B 15 ? ? ? ? 48 8D 0D ? ? ? ? 41";
    std::array<std::uint8_t, 1024> bytes{};
    bytes.fill(0xcc);
    for (std::size_t i = 0; i < 10; ++i) {
        const auto site = 0x20 + i * 0x30;
        for (std::size_t j = 0; j < instructionBytes.size(); ++j) {
            bytes[site + j] = instructionBytes[j];
        }
        write_displacement(bytes, site + 11, base, managerSlot);
    }
    const auto result = scan_references(bytes, base, signature, 11);
    check(result.matches == 10, "synthetic manager buffer contains ten matching sites");
    check(result.targets.distinct_count() == 1 && result.targets.unique() == managerSlot,
          "ten manager references share one unique destination");

    bytes.fill(0xcc);
    const auto empty = scan_references(bytes, base, signature, 11);
    check(empty.matches == 0 && empty.targets.distinct_count() == 0 &&
              !empty.targets.unique(), "zero pattern matches leaves symbol unresolved");
}

void test_direct_call_references() {
    constexpr std::uintptr_t base = 0x10000000;
    constexpr std::uintptr_t function = base + 0x80;
    constexpr std::array<std::uint8_t, 13> instructionBytes{
        0x41, 0xb8, 0x14, 0, 0, 0, 0x03, 0xd3, 0xe8, 0, 0, 0, 0};
    constexpr auto signature = "41 B8 14 00 00 00 03 D3 E8 ? ? ? ?";
    std::array<std::uint8_t, 1024> bytes{};
    bytes.fill(0xcc);
    const auto addCall = [&](std::size_t site) {
        for (std::size_t j = 0; j < instructionBytes.size(); ++j) {
            bytes[site + j] = instructionBytes[j];
        }
        write_displacement(bytes, site + 9, base, function);
    };
    addCall(0x20);
    addCall(0xa0);
    check(read_displacement(bytes.data() + 0x29) > 0 &&
              read_displacement(bytes.data() + 0xa9) < 0,
          "synthetic E8 fixture covers positive and negative signed rel32 fields");
    const auto result = scan_references(bytes, base, signature, 9);
    check(result.matches == 2 && result.targets.distinct_count() == 1 &&
              result.targets.unique() == function,
          "two E8 byte-pattern matches share one unique function");

    for (std::size_t i = 0; i < 8; ++i) {
        addCall(0x100 + i * 0x20);
    }
    const auto tenCalls = scan_references(bytes, base, signature, 9);
    check(tenCalls.matches == 10 && tenCalls.targets.unique() == function,
          "ten matching E8 calls initially agree");
    write_displacement(bytes, 0x1e9, base, base + 0x380);
    const auto conflicting = scan_references(bytes, base, signature, 9);
    check(conflicting.matches == 10 && conflicting.targets.distinct_count() == 2 &&
              !conflicting.targets.unique(),
          "mutating one valid call target refuses resolution despite a 9:1 majority");
}

void test_shutdown_signature_includes_call() {
    constexpr std::uintptr_t base = 0x10000000;
    constexpr std::array<std::uint8_t, 15> tail{
        0x75, 0x0f, 0xe8, 0, 0, 0, 0, 0x8b, 0x0d, 0, 0, 0, 0, 0x3b, 0xc8};
    std::array<std::uint8_t, 512> bytes{};
    bytes.fill(0xcc);
    for (const auto tailSite : {0x20U, 0x10cU}) {
        for (std::size_t j = 0; j < tail.size(); ++j) {
            bytes[tailSite + j] = tail[j];
        }
    }
    bytes[0x100] = 0xe8;
    write_displacement(bytes, 0x101, base, base + 0x180);
    const auto oldTail = scan_references(bytes, base,
        "75 0F E8 ? ? ? ? 8B 0D ? ? ? ? 3B C8", 3);
    const auto withCall = scan_references(bytes, base,
        "E8 ? ? ? ? ? ? ? ? ? ? ? 75 0F E8 ? ? ? ? 8B 0D ? ? ? ? 3B C8", 1);
    check(oldTail.matches == 2, "old shutdown tail matches one accidental site");
    check(withCall.matches == 1 && withCall.targets.unique() == base + 0x180,
          "shutdown signature requires its leading E8 and excludes tail-only match");
}

void test_address_boundaries() {
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    constexpr auto minimumDisplacement = std::numeric_limits<std::int32_t>::min();
    constexpr auto maximumDisplacement = std::numeric_limits<std::int32_t>::max();

    check(ymt::relative_target(0x1000, 0) == 0x1004,
          "zero displacement uses the end of the displacement field");
    check(ymt::relative_target(0, -4) == 0,
          "mathematically valid zero remains distinguishable from overflow");
    check(!ymt::relative_target(0, -5), "negative offset underflow is rejected");
    check(ymt::relative_target(maximum - 4, 0) == maximum,
          "end of displacement may equal maximum address");
    check(!ymt::relative_target(maximum - 4, 1),
          "positive offset overflow is rejected");
    check(!ymt::relative_target(maximum - 3, -4),
          "overflow of the displacement field end is rejected before delta");
    check(!ymt::relative_target(maximum, minimumDisplacement),
          "negative displacement cannot hide overflow of the field end");

    check(ymt::relative_target(0x7ffffffcU, minimumDisplacement) == 0,
          "INT32_MIN magnitude is computed without signed negation overflow");
    check(ymt::relative_target(0x80000000U, minimumDisplacement) == 4,
          "INT32_MIN resolves a nonzero in-range target");
    check(!ymt::relative_target(0x7ffffffbU, minimumDisplacement),
          "INT32_MIN rejects an underflow by one byte");
    check(ymt::relative_target(0, maximumDisplacement) == 0x80000003U,
          "INT32_MAX resolves without signed addition overflow");
    check(ymt::relative_target(maximum - 4 -
                                   static_cast<std::uintptr_t>(maximumDisplacement),
                               maximumDisplacement) == maximum,
          "INT32_MAX may land exactly on maximum address");
    check(!ymt::relative_target(maximum - 3 -
                                    static_cast<std::uintptr_t>(maximumDisplacement),
                                maximumDisplacement),
          "INT32_MAX overflow by one byte is rejected");
}

}  // namespace

int main() {
    test_empty_duplicates_and_ambiguity();
    test_streaming_manager_references();
    test_direct_call_references();
    test_shutdown_signature_includes_call();
    test_address_boundaries();
    if (failures != 0) {
        std::cerr << failures << " reference-target checks failed\n";
        return 1;
    }
    std::cout << "All reference-target checks passed\n";
    return 0;
}
