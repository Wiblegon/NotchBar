#include <windows.h>

#include "test.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);
    int failedCases = 0;
    for (auto& c : t::Registry()) {
        int before = t::Failures();
        c.fn();
        bool ok = t::Failures() == before;
        if (!ok) ++failedCases;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
    }
    std::printf("\n%zu tests, %d failed (%d checks failed)\n", t::Registry().size(), failedCases, t::Failures());
    return failedCases == 0 ? 0 : 1;
}
