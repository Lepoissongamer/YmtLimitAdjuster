#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ymt {

inline constexpr std::size_t kDependencyCapacity = 256;

enum class FilterStatus {
    ok,
    insufficient_resident,
    invalid_input,
    capacity_exceeded,
};

struct FilterResult {
    FilterStatus status;
    std::size_t written;
    std::size_t omitted;
};

// Copy the complete sequence when it already fits. Otherwise, omit only entries
// for which isResidentLoaded returns true. The predicate must establish BOTH
// permanent residency and completed loading; pending/reserved entries are unsafe
// to omit. It is called once per input entry, in order, only on the overflow path.
//
// Failure never writes output and returns zero counters. Empty input accepts null
// pointers. Nonempty input requires input; output may be null when capacity is
// zero. Input/output overlap is supported. Predicate exceptions propagate while
// leaving output untouched (provided the predicate itself does not mutate it).
template <typename IsResidentLoaded>
FilterResult filter_dependencies(const std::uint32_t* input,
                                 std::size_t total,
                                 std::uint32_t* output,
                                 std::size_t capacity,
                                 IsResidentLoaded&& isResidentLoaded) {
    if (total > kDependencyCapacity) {
        return {FilterStatus::capacity_exceeded, 0, 0};
    }
    if (total == 0) {
        return {FilterStatus::ok, 0, 0};
    }
    if (input == nullptr || (capacity != 0 && output == nullptr)) {
        return {FilterStatus::invalid_input, 0, 0};
    }

    // Stage the complete result before touching output. Apart from preserving
    // output on failure, this makes overlapping input/output ranges safe.
    std::array<std::uint32_t, kDependencyCapacity> staged{};
    std::size_t retained = 0;
    if (total <= capacity) {
        for (std::size_t i = 0; i < total; ++i) {
            staged[retained++] = input[i];
        }
    } else {
        for (std::size_t i = 0; i < total; ++i) {
            const auto dependency = input[i];
            if (!isResidentLoaded(dependency)) {
                staged[retained++] = dependency;
            }
        }
        if (retained > capacity) {
            return {FilterStatus::insufficient_resident, 0, 0};
        }
    }

    for (std::size_t i = 0; i < retained; ++i) {
        output[i] = staged[i];
    }
    return {FilterStatus::ok, retained, total - retained};
}

}  // namespace ymt
