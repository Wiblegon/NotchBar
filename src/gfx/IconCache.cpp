#include "pch.h"

#include "gfx/IconCache.h"

#include "core/Dispatcher.h"
#include "core/Worker.h"
#include "gfx/Graphics.h"

namespace nb::gfx {
namespace {
constexpr size_t kMaxEntries = 192;

struct Entry {
    std::vector<uint8_t> pixels;
    UINT w = 0, h = 0;
    winrt::com_ptr<ID2D1Bitmap> bmp;
    uint64_t gen = 0;
    bool loading = false, failed = false;
    uint64_t lastUse = 0;
};
std::unordered_map<std::wstring, Entry> g_entries;
std::function<void()> g_onLoaded;
uint64_t g_useCounter = 0;
bool g_notifyPosted = false;

bool PixelsFromWicSource(IWICBitmapSource* src, int px, std::vector<uint8_t>& out, UINT& w, UINT& h) {
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())))) return false;
    winrt::com_ptr<IWICBitmapSource> conv;
    if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, src, conv.put()))) return false;
    UINT sw = 0, sh = 0;
    conv->GetSize(&sw, &sh);
    winrt::com_ptr<IWICBitmapSource> final = conv;
    if (px > 0 && (sw > static_cast<UINT>(px) * 2 || sh > static_cast<UINT>(px) * 2)) {
        winrt::com_ptr<IWICBitmapScaler> scaler;
        wic->CreateBitmapScaler(scaler.put());
        if (scaler && SUCCEEDED(scaler->Initialize(conv.get(), px, px, WICBitmapInterpolationModeFant))) final = scaler;
    }
    final->GetSize(&w, &h);
    if (!w || !h || w > 512 || h > 512) return false;
    out.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(final->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data()));
}

bool FromHBitmap(HBITMAP hb, int px, std::vector<uint8_t>& out, UINT& w, UINT& h) {
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())))) return false;
    winrt::com_ptr<IWICBitmap> bmp;
    // Shell image factory bitmaps carry straight (non-premultiplied) alpha.
    if (FAILED(wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUseAlpha, bmp.put()))) return false;
    return PixelsFromWicSource(bmp.get(), px, out, w, h);
}

bool FromHIcon(HICON hi, int px, std::vector<uint8_t>& out, UINT& w, UINT& h) {
    winrt::com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())))) return false;
    winrt::com_ptr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmapFromHICON(hi, bmp.put()))) return false;
    return PixelsFromWicSource(bmp.get(), px, out, w, h);
}

bool FromShellItem(IShellItem* item, int px, std::vector<uint8_t>& out, UINT& w, UINT& h) {
    winrt::com_ptr<IShellItemImageFactory> f;
    if (FAILED(item->QueryInterface(IID_PPV_ARGS(f.put())))) return false;
    HBITMAP hb = nullptr;
    if (FAILED(f->GetImage({px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb) return false;
    bool ok = FromHBitmap(hb, px, out, w, h);
    DeleteObject(hb);
    return ok;
}

// Runs on the STA shell worker.
bool Load(const std::wstring& key, int px, std::vector<uint8_t>& out, UINT& w, UINT& h) {
    auto colon = key.find(L':');
    if (colon == std::wstring::npos) return false;
    std::wstring kind = key.substr(0, colon), arg = key.substr(colon + 1);
    if (kind == L"app") {
        winrt::com_ptr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, arg.c_str(), IID_PPV_ARGS(item.put()))))
            return FromShellItem(item.get(), px, out, w, h);
        return false;
    }
    if (kind == L"file") {
        winrt::com_ptr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(arg.c_str(), nullptr, IID_PPV_ARGS(item.put()))))
            return FromShellItem(item.get(), px, out, w, h);
        return false;
    }
    if (kind == L"hwnd") {
        HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(wcstoull(arg.c_str(), nullptr, 16)));
        DWORD_PTR res = 0;
        HICON icon = nullptr;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, px > 24 ? ICON_BIG : ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 150, &res) && res)
            icon = reinterpret_cast<HICON>(res);
        if (!icon) icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, px > 24 ? GCLP_HICON : GCLP_HICONSM));
        if (!icon) return false;
        return FromHIcon(icon, px, out, w, h);
    }
    return false;
}

void Evict() {
    while (g_entries.size() > kMaxEntries) {
        auto oldest = g_entries.end();
        for (auto it = g_entries.begin(); it != g_entries.end(); ++it)
            if (!it->second.loading && (oldest == g_entries.end() || it->second.lastUse < oldest->second.lastUse)) oldest = it;
        if (oldest == g_entries.end()) break;
        g_entries.erase(oldest);
    }
}
} // namespace

IconCache& IconCache::Get() {
    static IconCache c;
    return c;
}

void IconCache::SetOnLoaded(std::function<void()> fn) { g_onLoaded = std::move(fn); }

ID2D1Bitmap* IconCache::Get(const std::wstring& key, int px) {
    if (key.empty()) return nullptr;
    std::wstring k = key + L"|" + std::to_wstring(px);
    auto& e = g_entries[k];
    e.lastUse = ++g_useCounter;
    if (e.failed) return nullptr;
    auto& g = Graphics::Get();
    if (!e.pixels.empty()) {
        if (!e.bmp || e.gen != g.Generation()) {
            e.bmp = nullptr;
            D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (g.ResourceContext())
                g.ResourceContext()->CreateBitmap({e.w, e.h}, e.pixels.data(), e.w * 4, props, e.bmp.put());
            e.gen = g.Generation();
        }
        return e.bmp.get();
    }
    if (!e.loading) {
        e.loading = true;
        WorkerPool::Shell().Submit([k, key, px] {
            std::vector<uint8_t> pixels;
            UINT w = 0, h = 0;
            bool ok = Load(key, px, pixels, w, h);
            ui::Post([k, ok, pixels = std::move(pixels), w, h]() mutable {
                auto it = g_entries.find(k);
                if (it == g_entries.end()) return;
                it->second.loading = false;
                if (!ok) {
                    it->second.failed = true;
                    return;
                }
                it->second.pixels = std::move(pixels);
                it->second.w = w;
                it->second.h = h;
                // Coalesce: one repaint for a burst of icon loads.
                if (!g_notifyPosted) {
                    g_notifyPosted = true;
                    ui::Post([] {
                        g_notifyPosted = false;
                        if (g_onLoaded) g_onLoaded();
                    });
                }
            });
        });
        Evict();
    }
    return nullptr;
}

void IconCache::Forget(const std::wstring& prefix) {
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (it->first.rfind(prefix, 0) == 0 && !it->second.loading) it = g_entries.erase(it);
        else ++it;
    }
}

size_t IconCache::Count() const { return g_entries.size(); }
void IconCache::Clear() { g_entries.clear(); }

} // namespace nb::gfx
