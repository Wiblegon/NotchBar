#include "pch.h"

#include "sys/FileSearch.h"

#include <oledb.h>
#include <oledberr.h>

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Worker.h"

namespace nb::sys {
namespace {
// DBGUID_DEFAULT {C8B521FB-5CF3-11CE-ADE5-00AA0044773D}
const GUID kDbGuidDefault = {0xc8b521fb, 0x5cf3, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};
constexpr int kMaxResults = 12;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

std::vector<std::wstring> Words(const std::wstring& q) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : q) {
        // Characters with meaning in the query language are dropped, never passed through.
        if (c == L'\'' || c == L'"' || c == L'*' || c == L'%' || c == L'\\' || c == L'(' || c == L')' || c == L'[' || c == L']' ||
            c == L';')
            c = L' ';
        if (iswspace(c)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    if (out.size() > 6) out.resize(6);
    return out;
}

void Fill(FileHit& h) {
    auto slash = h.path.find_last_of(L'\\');
    h.folder = slash == std::wstring::npos ? L"" : h.path.substr(0, slash);
    if (h.name.empty()) h.name = slash == std::wstring::npos ? h.path : h.path.substr(slash + 1);
}

bool IndexQuery(const std::wstring& text, std::vector<FileHit>& out) {
    auto words = Words(text);
    if (words.empty()) return true;
    CLSID clsid;
    if (FAILED(CLSIDFromProgID(L"Search.CollatorDSO", &clsid))) return false;
    winrt::com_ptr<IDBInitialize> init;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(init.put())))) return false;
    if (FAILED(init->Initialize())) return false;
    auto cs = init.try_as<IDBCreateSession>();
    if (!cs) return false;
    winrt::com_ptr<IDBCreateCommand> cc;
    if (FAILED(cs->CreateSession(nullptr, IID_IDBCreateCommand, reinterpret_cast<IUnknown**>(cc.put())))) return false;
    winrt::com_ptr<ICommandText> cmd;
    if (FAILED(cc->CreateCommand(nullptr, IID_ICommandText, reinterpret_cast<IUnknown**>(cmd.put())))) return false;

    std::wstring profile = KnownFolder(FOLDERID_Profile);
    std::replace(profile.begin(), profile.end(), L'\\', L'/');
    std::wstring contains;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) contains += L" AND ";
        contains += L"\"" + words[i] + L"*\"";
    }
    std::wstring sql = L"SELECT TOP " + std::to_wstring(kMaxResults) +
                       L" System.ItemPathDisplay, System.ItemNameDisplay, System.ItemType FROM SystemIndex WHERE SCOPE='file:" +
                       profile + L"' AND CONTAINS(System.ItemNameDisplay, '" + contains +
                       L"') AND NOT System.ItemPathDisplay LIKE '%\\AppData\\%' ORDER BY System.Search.Rank DESC";
    if (FAILED(cmd->SetCommandText(kDbGuidDefault, sql.c_str()))) return false;
    winrt::com_ptr<IRowset> rs;
    HRESULT hr = cmd->Execute(nullptr, IID_IRowset, nullptr, nullptr, reinterpret_cast<IUnknown**>(rs.put()));
    if (FAILED(hr) || !rs) return hr == DB_E_NOTABLE ? false : SUCCEEDED(hr);

    struct Row {
        DBSTATUS s1;
        wchar_t path[1024];
        DBSTATUS s2;
        wchar_t name[260];
        DBSTATUS s3;
        wchar_t type[64];
    };
    DBBINDING b[3]{};
    auto bind = [](DBBINDING& x, DBORDINAL ord, size_t val, size_t st, size_t len) {
        x.iOrdinal = ord;
        x.obValue = val;
        x.obStatus = st;
        x.dwPart = DBPART_VALUE | DBPART_STATUS;
        x.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        x.eParamIO = DBPARAMIO_NOTPARAM;
        x.cbMaxLen = len;
        x.wType = DBTYPE_WSTR;
    };
    bind(b[0], 1, offsetof(Row, path), offsetof(Row, s1), sizeof(Row::path));
    bind(b[1], 2, offsetof(Row, name), offsetof(Row, s2), sizeof(Row::name));
    bind(b[2], 3, offsetof(Row, type), offsetof(Row, s3), sizeof(Row::type));
    auto acc = rs.as<IAccessor>();
    HACCESSOR ha = 0;
    if (FAILED(acc->CreateAccessor(DBACCESSOR_ROWDATA, 3, b, sizeof(Row), &ha, nullptr))) return false;
    HROW rows[kMaxResults];
    HROW* prow = rows;
    DBCOUNTITEM got = 0;
    if (SUCCEEDED(rs->GetNextRows(DB_NULL_HCHAPTER, 0, kMaxResults, &got, &prow))) {
        for (DBCOUNTITEM i = 0; i < got; ++i) {
            Row r{};
            if (SUCCEEDED(rs->GetData(rows[i], ha, &r)) && r.s1 == DBSTATUS_S_OK) {
                FileHit h;
                h.path = r.path;
                if (r.s2 == DBSTATUS_S_OK) h.name = r.name;
                h.isFolder = r.s3 == DBSTATUS_S_OK && (_wcsicmp(r.type, L"Directory") == 0 || _wcsicmp(r.type, L"File folder") == 0);
                Fill(h);
                out.push_back(std::move(h));
            }
        }
        rs->ReleaseRows(got, rows, nullptr, nullptr, nullptr);
    }
    acc->ReleaseAccessor(ha, nullptr);
    return true;
}

// ---- limited fallback ----
std::mutex g_fbMu;
std::vector<FileHit> g_fb;
ULONGLONG g_fbBuilt = 0;

void Scan(const std::wstring& dir, int depth, std::vector<FileHit>& out) {
    if (depth < 0 || out.size() >= 5000) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        FileHit fh;
        fh.path = dir + L"\\" + fd.cFileName;
        fh.name = fd.cFileName;
        fh.folder = dir;
        fh.isFolder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out.push_back(fh);
        if (fh.isFolder && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Scan(fh.path, depth - 1, out);
    } while (out.size() < 5000 && FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<FileHit> FallbackQuery(const std::wstring& text) {
    std::lock_guard lk(g_fbMu);
    ULONGLONG now = GetTickCount64();
    if (g_fb.empty() || now - g_fbBuilt > 10 * 60 * 1000) {
        g_fb.clear();
        for (auto id : {FOLDERID_Desktop, FOLDERID_Documents, FOLDERID_Downloads}) Scan(KnownFolder(id), 3, g_fb);
        g_fbBuilt = now;
    }
    std::vector<std::wstring> words;
    for (auto& w : Words(text)) words.push_back(search::Fold(w));
    std::vector<FileHit> out;
    for (const auto& f : g_fb) {
        std::wstring n = search::Fold(f.name);
        bool all = std::all_of(words.begin(), words.end(), [&](const std::wstring& w) { return n.find(w) != std::wstring::npos; });
        if (all) out.push_back(f);
        if (out.size() >= kMaxResults) break;
    }
    return out;
}
} // namespace

FileSearch& FileSearch::Get() {
    static FileSearch f;
    return f;
}

void FileSearch::Query(const std::wstring& text, const search::QueryGate::Ticket& ticket,
                       std::function<void(std::vector<FileHit>, FileSource)> done) {
    WorkerPool::Background().Submit(
        [text, ticket, done] {
            if (ticket.IsCancelled()) return; // superseded before it started
            std::vector<FileHit> hits;
            FileSource src = FileSource::Index;
            if (!IndexQuery(text, hits)) {
                hits = FallbackQuery(text);
                src = FileSource::Fallback;
            }
            if (ticket.IsCancelled()) return;
            ui::Post([hits = std::move(hits), src, done]() mutable { done(std::move(hits), src); });
        },
        "file-search"); // a newer query replaces a queued older one
}

void FileSearch::Open(const FileHit& h) {
    ShellExecuteW(nullptr, L"open", h.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void FileSearch::ShowInFolder(const FileHit& h) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(h.path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

} // namespace nb::sys
