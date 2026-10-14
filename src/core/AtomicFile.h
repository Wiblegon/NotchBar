#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace nb::file {

// Writes to "<path>.tmp", flushes to disk, then atomically renames over <path>.
// A crash or power loss leaves either the old or the new file, never a torn one.
bool WriteAtomic(const std::wstring& path, std::string_view data);

std::optional<std::string> Read(const std::wstring& path, size_t maxBytes = 8u << 20);

// Moves an unreadable file aside ("<path>.corrupt-<timestamp>") so it can be inspected and is
// not overwritten by defaults. Returns the new name.
std::wstring Quarantine(const std::wstring& path);

bool EnsureDir(const std::wstring& dir);

} // namespace nb::file
