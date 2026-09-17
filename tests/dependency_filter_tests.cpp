#include "ymt/dependency_filter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

void check_result(const ymt::FilterResult& result, ymt::FilterStatus status,
                  std::size_t written, std::size_t omitted,
                  const char* description) {
    check(result.status == status && result.written == written &&
              result.omitted == omitted,
          description);
}

void test_empty_and_invalid_inputs() {
    std::size_t calls = 0;
    const auto resident = [&calls](std::uint32_t) {
        ++calls;
        return true;
    };
    check_result(ymt::filter_dependencies(nullptr, 0, nullptr, 0, resident),
                 ymt::FilterStatus::ok, 0, 0, "empty null ranges accepted");
    check_result(ymt::filter_dependencies(nullptr, 0, nullptr, 256, resident),
                 ymt::FilterStatus::ok, 0, 0, "empty input needs no output");

    std::array<std::uint32_t, 3> guarded{0x1234, 0x5678, 0x9abc};
    const auto before = guarded;
    check_result(ymt::filter_dependencies(nullptr, 1, &guarded[1], 1, resident),
                 ymt::FilterStatus::invalid_input, 0, 0, "null input rejected");
    check(guarded == before, "invalid input leaves output and canaries intact");
    const std::uint32_t input = 42;
    check_result(ymt::filter_dependencies(&input, 1, nullptr, 1, resident),
                 ymt::FilterStatus::invalid_input, 0, 0, "null output rejected");
    check_result(ymt::filter_dependencies(nullptr, 1, nullptr, 0, resident),
                 ymt::FilterStatus::invalid_input, 0, 0,
                 "zero capacity does not permit a missing nonempty input");
    check(calls == 0, "empty and invalid input never invoke predicate");
}

void test_fast_path() {
    const std::array<std::uint32_t, 5> input{3, 1, 3, 0, 0xffffffffu};
    std::array<std::uint32_t, 8> output{};
    output.fill(0xfeedbeef);
    std::size_t calls = 0;
    const auto resident = [&calls](std::uint32_t) {
        ++calls;
        return true;
    };
    const auto result = ymt::filter_dependencies(
        input.data(), input.size(), output.data() + 1, 6, resident);
    check_result(result, ymt::FilterStatus::ok, 5, 0,
                 "fitting sequence is copied without any omissions");
    for (std::size_t i = 0; i < input.size(); ++i) {
        check(output[i + 1] == input[i], "fast path preserves values and order");
    }
    check(output.front() == 0xfeedbeef && output[6] == 0xfeedbeef &&
              output.back() == 0xfeedbeef,
          "fast path writes only its result");
    check(calls == 0, "fast path never invokes resident predicate");
}

void test_resident_and_loaded_requirement() {
    struct State {
        bool resident;
        bool loaded;
    };
    const std::array<State, 4> states{{{true, true}, {true, false},
                                     {false, true}, {false, false}}};
    const std::array<std::uint32_t, 6> input{0, 1, 2, 3, 0, 1};
    std::array<std::uint32_t, 6> output{};
    output.fill(0xfeedbeef);
    std::vector<std::uint32_t> visited;
    const auto resident = [&states, &visited](std::uint32_t id) {
        visited.push_back(id);
        return states[id].resident && states[id].loaded;
    };
    const auto result = ymt::filter_dependencies(
        input.data(), input.size(), output.data() + 1, 4, resident);
    check_result(result, ymt::FilterStatus::ok, 4, 2,
                 "only resident and fully loaded entries can be omitted");
    check(output == std::array<std::uint32_t, 6>{0xfeedbeef, 1, 2, 3, 1, 0xfeedbeef},
          "pending entries, nonresident entries, duplicates and order preserved");
    check(visited == std::vector<std::uint32_t>(input.begin(), input.end()),
          "overflow predicate visits every occurrence exactly once in order");
}

void test_failure_is_transactional() {
    const std::array<std::uint32_t, 5> input{10, 20, 30, 40, 50};
    std::array<std::uint32_t, 5> guarded{0x1111, 0x2222, 0x3333, 0x4444, 0x5555};
    const auto before = guarded;
    const auto result = ymt::filter_dependencies(
        input.data(), input.size(), guarded.data() + 1, 2,
        [](std::uint32_t id) { return id == 10 || id == 50; });
    check_result(result, ymt::FilterStatus::insufficient_resident, 0, 0,
                 "too few removable entries fails with no committed counts");
    check(guarded == before, "capacity failure leaves whole output unchanged");

    bool threw = false;
    try {
        ymt::filter_dependencies(
            input.data(), input.size(), guarded.data() + 1, 2,
            [](std::uint32_t id) {
                if (id == 40) {
                    throw std::runtime_error("predicate failure");
                }
                return id == 10;
            });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "predicate exception propagates");
    check(guarded == before, "predicate exception leaves whole output unchanged");
}

void test_zero_capacity() {
    const std::array<std::uint32_t, 3> input{4, 5, 6};
    check_result(ymt::filter_dependencies(input.data(), input.size(), nullptr, 0,
                                          [](std::uint32_t) { return true; }),
                 ymt::FilterStatus::ok, 0, 3,
                 "zero capacity succeeds when every entry is safe to omit");
    check_result(ymt::filter_dependencies(input.data(), input.size(), nullptr, 0,
                                          [](std::uint32_t id) { return id != 5; }),
                 ymt::FilterStatus::insufficient_resident, 0, 0,
                 "zero capacity rejects even one unsafe dependency");
}

void test_capacity_boundaries() {
    std::array<std::uint32_t, ymt::kDependencyCapacity> input{};
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::uint32_t>(i);
    }
    std::array<std::uint32_t, ymt::kDependencyCapacity + 2> output{};
    output.fill(0xfeedbeef);
    std::size_t calls = 0;
    auto resident = [&calls](std::uint32_t id) {
        ++calls;
        return id == 128;
    };
    check_result(ymt::filter_dependencies(input.data(), 256, output.data() + 1,
                                          256, resident),
                 ymt::FilterStatus::ok, 256, 0, "exact 256-entry input is valid");
    check(calls == 0, "exact fit at upper bound uses fast path");
    for (std::size_t i = 0; i < input.size(); ++i) {
        check(output[i + 1] == input[i], "upper-bound fast copy is complete");
    }
    check(output.front() == 0xfeedbeef && output.back() == 0xfeedbeef,
          "upper-bound copy preserves canaries");

    output.fill(0xfeedbeef);
    check_result(ymt::filter_dependencies(input.data(), 256, output.data() + 1,
                                          255, resident),
                 ymt::FilterStatus::ok, 255, 1,
                 "256-entry input fits smaller output after one safe omission");
    check(calls == 256, "upper-bound overflow checks all entries");
    for (std::size_t i = 0; i < 255; ++i) {
        check(output[i + 1] == i + (i >= 128 ? 1 : 0),
              "upper-bound filtering preserves order around omission");
    }
    check(output.front() == 0xfeedbeef && output[256] == 0xfeedbeef &&
              output.back() == 0xfeedbeef,
          "upper-bound filtered copy preserves canaries and spare capacity");

    const auto before = output;
    check_result(ymt::filter_dependencies(input.data(), 257, output.data() + 1,
                                          256, resident),
                 ymt::FilterStatus::capacity_exceeded, 0, 0,
                 "257-entry report is rejected before reading input");
    check_result(ymt::filter_dependencies(nullptr,
                                          std::numeric_limits<std::size_t>::max(),
                                          nullptr, 0, resident),
                 ymt::FilterStatus::capacity_exceeded, 0, 0,
                 "extreme totals rejected without pointer access or arithmetic overflow");
    check(calls == 256, "oversize inputs never invoke predicate");
    check(output == before, "oversize rejection leaves output unchanged");
}

void test_overlapping_ranges() {
    std::array<std::uint32_t, 6> buffer{1, 2, 3, 4, 5, 0xfeedbeef};
    check_result(ymt::filter_dependencies(buffer.data(), 5, buffer.data() + 1, 5,
                                          [](std::uint32_t) { return false; }),
                 ymt::FilterStatus::ok, 5, 0, "forward overlap succeeds");
    check(buffer == std::array<std::uint32_t, 6>{1, 1, 2, 3, 4, 5},
          "forward overlap does not overwrite unread dependencies");

    buffer = {1, 2, 3, 4, 5, 0xfeedbeef};
    check_result(ymt::filter_dependencies(buffer.data(), 5, buffer.data(), 3,
                                          [](std::uint32_t id) { return id % 2 == 0; }),
                 ymt::FilterStatus::ok, 3, 2, "in-place overflow filtering succeeds");
    check(buffer == std::array<std::uint32_t, 6>{1, 3, 5, 4, 5, 0xfeedbeef},
          "in-place filtering writes only retained entries");
}

void test_all_small_residency_patterns() {
    // An exhaustive property check includes duplicate IDs and varied capacities.
    const std::array<std::uint32_t, 7> input{2, 0, 3, 2, 1, 0, 3};
    for (unsigned mask = 0; mask < 16; ++mask) {
        for (std::size_t capacity = 0; capacity <= input.size() + 1; ++capacity) {
            std::vector<std::uint32_t> expected;
            for (const auto id : input) {
                if (input.size() <= capacity || (mask & (1u << id)) == 0) {
                    expected.push_back(id);
                }
            }
            std::array<std::uint32_t, 10> output{};
            output.fill(0xfeedbeef);
            const auto before = output;
            const auto result = ymt::filter_dependencies(
                input.data(), input.size(), output.data() + 1, capacity,
                [mask](std::uint32_t id) { return (mask & (1u << id)) != 0; });
            if (expected.size() > capacity) {
                check_result(result, ymt::FilterStatus::insufficient_resident, 0, 0,
                             "exhaustive case fails when unsafe entries do not fit");
                check(output == before, "exhaustive failure is transactional");
            } else {
                check_result(result, ymt::FilterStatus::ok, expected.size(),
                             input.size() - expected.size(),
                             "exhaustive success counts match reference sequence");
                for (std::size_t i = 0; i < expected.size(); ++i) {
                    check(output[i + 1] == expected[i],
                          "exhaustive success retains reference sequence");
                }
                check(output.front() == 0xfeedbeef, "exhaustive prefix canary intact");
                for (std::size_t i = expected.size() + 1; i < output.size(); ++i) {
                    check(output[i] == 0xfeedbeef, "exhaustive output tail untouched");
                }
            }
        }
    }
}

}  // namespace

int main() {
    test_empty_and_invalid_inputs();
    test_fast_path();
    test_resident_and_loaded_requirement();
    test_failure_is_transactional();
    test_zero_capacity();
    test_capacity_boundaries();
    test_overlapping_ranges();
    test_all_small_residency_patterns();
    if (failures != 0) {
        std::cerr << failures << " dependency-filter checks failed\n";
        return 1;
    }
    std::cout << "All dependency-filter checks passed\n";
    return 0;
}
