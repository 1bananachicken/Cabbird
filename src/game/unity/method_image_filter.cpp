#include "cabbird/unity_services.hpp"

#include <string>
#include <vector>

namespace cabbird {

std::vector<std::string> SplitMethodImageFilter(const char* text, std::uint32_t size) {
    std::vector<std::string> needles;
    if (text == nullptr || size == 0) return needles;

    std::string current;
    const auto flush = [&needles, &current] {
        if (!current.empty()) {
            needles.push_back(current);
            current.clear();
        }
    };
    for (std::uint32_t index = 0; index < size; ++index) {
        const char ch = text[index];
        if (ch == ',' || ch == ' ' || ch == '\t' || ch == ';' || ch == '|') {
            flush();
            continue;
        }
        current.push_back(ch);
    }
    flush();
    return needles;
}

}  // namespace cabbird
