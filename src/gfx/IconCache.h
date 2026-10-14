#pragma once
// Bounded LRU cache of real Windows icons as D2D bitmaps. Icons are extracted on the shell
// worker thread (never on the UI thread); pixels are kept so device loss only re-uploads.
// Key formats:
//   "app:<AppsFolder parsing name>"  packaged + Start-menu apps
//   "file:<path>"                    executables / documents
//   "hwnd:<hex handle>"              window icon (fallback)
#include <d2d1_3.h>

#include <functional>
#include <string>

namespace nb::gfx {

class IconCache {
public:
    static IconCache& Get();
    // Returns nullptr until loaded; the load is queued once and onLoaded fires when it lands.
    ID2D1Bitmap* Get(const std::wstring& key, int pixelSize);
    void SetOnLoaded(std::function<void()> fn);
    void Forget(const std::wstring& keyPrefix);
    size_t Count() const;
    void Clear();
};

} // namespace nb::gfx
