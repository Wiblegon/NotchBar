#pragma once
// File search through the Windows Search index (OLE DB provider "Search.CollatorDSO"), so a
// keystroke never walks the disk. If the indexer is unavailable (service disabled), a limited
// fallback indexes only Desktop, Documents and Downloads (depth 3, max 5000 entries, rebuilt at
// most every 10 minutes) and says so in the UI.
#include <functional>
#include <string>
#include <vector>

#include "model/Search.h"

namespace nb::sys {

struct FileHit {
    std::wstring path;
    std::wstring name;
    std::wstring folder;
    bool isFolder = false;
};

enum class FileSource { Index, Fallback, Unavailable };

class FileSearch {
public:
    static FileSearch& Get();
    // Runs on a worker; `done` is called on the UI thread. The caller checks the ticket.
    void Query(const std::wstring& text, const search::QueryGate::Ticket& ticket,
               std::function<void(std::vector<FileHit>, FileSource)> done);
    static void Open(const FileHit& h);
    static void ShowInFolder(const FileHit& h);
};

} // namespace nb::sys
