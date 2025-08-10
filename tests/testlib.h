#pragma once

#include <string>
#include <unordered_set>

inline bool match(const std::string& filter, const std::string& str) {
    size_t fi = 0, si = 0, star = std::string::npos, match = 0;
    while (si < str.size()) {
        if (fi < filter.size() && (filter[fi] == '?' || filter[fi] == str[si])) {
            fi++; si++;
        } else if (fi < filter.size() && filter[fi] == '*') {
            star = fi++;
            match = si;
        } else if (star != std::string::npos) {
            fi = star + 1;
            si = ++match;
        } else {
            return false;
        }
    }
    while (fi < filter.size() && filter[fi] == '*') fi++;
    return fi == filter.size();
}

inline bool match_any(const std::unordered_set<std::string>& filters, const std::string& str) {
    if (filters.empty()) {
        return true;
    }

    for (const auto& filter : filters) {
        if (match(filter, str)) {
            return true;
        }
    }
    return false;
}

inline void parse_filters(int argc, char* argv[], std::unordered_set<std::string>& filters) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--filter")) {
            if (i + 1 < argc) {
                std::string filter(argv[++i]);
                size_t pos = 0;
                while (pos != std::string::npos) {
                    size_t next = filter.find(',', pos);
                    std::string sub = filter.substr(pos, next - pos);
                    if (!sub.empty()) {
                        filters.insert(sub);
                    }
                    pos = next == std::string::npos
                        ? next
                        : next + 1;
                }
            }
        }
    }
}

#define ADD_TEST(f, n, ...) \
    do { \
        const struct CMUnitTest tmp[] = { \
            f(n, ##__VA_ARGS__) \
        }; \
        for (int i = 0; i < sizeof(tmp) / sizeof(tmp[0]); i++) { \
            if (match_any(filters, tmp[i].name)) { \
                tests.emplace_back(tmp[i]); \
            } \
        } \
    } while (0);
