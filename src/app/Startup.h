#pragma once
// Startup timing: process creation and user logon are taken from the OS, stages are marked
// with QueryPerformanceCounter and written to startup.log (no personal data).
#include <string>

namespace nb::startup {

void Begin();
void Mark(const char* stage);
void Report(bool fromAutostart);

} // namespace nb::startup
