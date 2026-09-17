#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ymt {
class Pattern {
public:
    explicit Pattern(std::string_view text) {
        while (!text.empty()) {
            const auto start = text.find_first_not_of(" \t\n\r");
            if (start == text.npos) break;
            text.remove_prefix(start);
            const auto end = text.find_first_of(" \t\n\r");
            const auto token = text.substr(0, end);
            if (token == "?" || token == "??") bytes_.push_back(-1);
            else {
                if (token.size() != 2) throw std::invalid_argument("Invalid pattern token");
                bytes_.push_back(hex(token[0]) * 16 + hex(token[1]));
            }
            if (end == text.npos) break;
            text.remove_prefix(end);
        }
        if (bytes_.empty()) throw std::invalid_argument("Empty pattern");
    }
    std::size_t size() const { return bytes_.size(); }
    bool matches(const std::uint8_t* data, std::size_t length) const {
        if (!data || length < size()) return false;
        for (std::size_t i = 0; i < size(); ++i)
            if (bytes_[i] >= 0 && data[i] != bytes_[i]) return false;
        return true;
    }
private:
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::invalid_argument("Invalid hexadecimal pattern");
    }
    std::vector<int> bytes_;
};
}
