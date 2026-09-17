#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace ymt {

// Resolve an x86 rel32/RIP-relative displacement from the address of its first
// byte. This performs address arithmetic only: the caller must validate the
// instruction, readable displacement bytes, and resulting target's memory range.
// A mathematically valid zero target is returned here; ReferenceTargets ignores
// it because null is never a usable function or data target.
inline std::optional<std::uintptr_t> relative_target(
    std::uintptr_t displacementAddress, std::int32_t displacement) {
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    constexpr std::uintptr_t displacementSize = 4;
    if (displacementAddress > maximum - displacementSize) {
        return std::nullopt;
    }
    const auto nextInstruction = displacementAddress + displacementSize;

    if (displacement < 0) {
        // Widen before negation so INT32_MIN has a representable magnitude.
        const auto magnitude = static_cast<std::uintmax_t>(
            -static_cast<std::int64_t>(displacement));
        if (magnitude > nextInstruction) {
            return std::nullopt;
        }
        return nextInstruction - static_cast<std::uintptr_t>(magnitude);
    }

    const auto distance = static_cast<std::uintmax_t>(displacement);
    if (distance > maximum - nextInstruction) {
        return std::nullopt;
    }
    return nextInstruction + static_cast<std::uintptr_t>(distance);
}

// Collect nonzero decoded destinations. The platform resolver must separately
// retain all instruction/memory validation failures: a unique destination alone
// does not prove that it is usable. Competing destinations remain ambiguous.
class ReferenceTargets {
public:
    void add(std::uintptr_t target) {
        if (target != 0 &&
            std::find(targets_.begin(), targets_.end(), target) == targets_.end()) {
            targets_.push_back(target);
        }
    }

    std::optional<std::uintptr_t> unique() const {
        if (targets_.size() != 1) {
            return std::nullopt;
        }
        return targets_.front();
    }

    std::size_t distinct_count() const { return targets_.size(); }

private:
    std::vector<std::uintptr_t> targets_;
};

}  // namespace ymt
