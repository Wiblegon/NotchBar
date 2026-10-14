#pragma once
// Tiny dependency-free test harness.
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {
struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& Registry() {
    static std::vector<Case> r;
    return r;
}
inline int& Failures() {
    static int f = 0;
    return f;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { Registry().push_back({n, std::move(f)}); }
};
} // namespace t

#define TEST(name)                                   \
    static void name();                              \
    static t::Reg reg_##name(#name, name);           \
    static void name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            ++t::Failures();                                                           \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
