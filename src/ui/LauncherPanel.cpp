#include "pch.h"

#include "ui/LauncherPanel.h"

#include "app/App.h"
#include "core/Dispatcher.h"
#include "core/Paths.h"
#include "core/Store.h"
#include "core/Strings.h"
#include "gfx/Draw.h"
#include "gfx/IconCache.h"
#include "gfx/Theme.h"
#include "model/Calc.h"
#include "model/WebSearch.h"
#include "sys/AppCatalog.h"
#include "sys/Autostart.h"
#include "sys/NotificationMonitor.h"
#include "sys/battery/BatteryService.h"
#include "ui/Widgets.h"

namespace nb::ui {
using namespace theme;
using draw::Align;
namespace g = draw::glyph;

namespace {
constexpr float kM = 12;       // shadow margin
constexpr float kHeaderH = 44;
constexpr int kCols = 5;
constexpr float kTileH = 80;

void CopyToClipboard(HWND owner, const std::wstring& s) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

wchar_t DecimalSep() {
    wchar_t buf[8] = L".";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, buf, 8);
    return buf[0] ? buf[0] : L'.';
}

void Toggle(ID2D1DeviceContext* dc, D2D1_POINT_2F c, bool on, bool enabled) {
    D2D1_RECT_F r{c.x - 17, c.y - 10, c.x + 17, c.y + 10};
    draw::RoundRect(dc, r, 10, on ? (enabled ? kAccent : Rgba(120, 140, 170)) : Rgba(70, 72, 78));
    float kx = on ? r.right - 10 : r.left + 10;
    dc->FillEllipse(D2D1::Ellipse({kx, c.y}, 7, 7), draw::Brush(dc, enabled ? Rgba(250, 250, 252) : Rgba(170, 170, 175)));
}
} // namespace

struct LauncherPanel::Hit {
    D2D1_RECT_F r{};
    bool scrolls = true;
    bool selectable = false;
    int drag = -1; // grid item index (root or folder) for drag & drop
    std::function<void(ID2D1DeviceContext*, D2D1_RECT_F, bool hot, bool sel)> paint;
    std::function<void()> click;
    std::function<void(POINT)> menu;
};

struct LauncherPanel::Impl {
    LauncherPanel* self = nullptr;
    PanelDir dir = PanelDir::Below;
    D2D1_RECT_F pr{};               // panel rect (DIP, window coords)
    float gap = kPanelGap;
    gfx::Spring appear = gfx::FadeSpring(0);
    gfx::Spring scroll = gfx::SnappySpring(0);
    float contentH = 0, viewTop = 0, viewBottom = 0;
    std::vector<Hit> hits;
    int hot = -1, sel = -1;
    bool keyboard = false;
    std::wstring folderId;
    bool addMode = false;
    // results
    std::wstring query;
    calc::Result calcRes;
    std::vector<sys::AppEntry> apps;
    std::vector<sys::FileHit> files;
    sys::FileSource fileSrc = sys::FileSource::Index;
    bool filesPending = false;
    TimerId fileTimer = 0;
    bool copied = false;
    // drag
    int downHit = -1;
    PointF downPt{}, dragPt{};
    bool dragging = false;
    int dragIdx = -1, dropInsert = -1, dropMerge = -1;
    // inline rename
    std::unique_ptr<TextInput> rename;
    std::wstring renamingFolder;

    void Rebuild();
    void BuildHeader(const std::wstring& title, bool back, std::function<void()> onBack);
    void HeaderButton(float& x, wchar_t glyph, const wchar_t* label, std::function<void()> click);
    void BuildGrid(const std::vector<FavItem>& items, bool inFolder);
    void BuildAllApps();
    void BuildSettings();
    void BuildResults();
    void Launch(const std::wstring& id);
    void MenuForTile(int index, bool inFolder, POINT at);
    void StartRename(const std::wstring& folderId);
    void CommitRename(bool commit);
    void FinishDrop();
    int HitAt(PointF p) const;
    D2D1_RECT_F Screen(const Hit& h) const {
        float dy = h.scrolls ? -scroll.value : 0;
        return {h.r.left, h.r.top + dy, h.r.right, h.r.bottom + dy};
    }
    std::vector<int> Selectable() const {
        std::vector<int> s;
        for (size_t i = 0; i < hits.size(); ++i)
            if (hits[i].selectable) s.push_back(static_cast<int>(i));
        return s;
    }
    void EnsureVisible(int i);
    void Changed() {
        Store::Get().SaveFavorites();
        Store::Get().NotifyChanged();
    }
};

// ============================================================================ setup & geometry

bool LauncherPanel::Init() {
    if (!Create(L"NotchBar uygulama paneli")) return false;
    d_ = std::make_shared<Impl>();
    d_->self = this;
    return true;
}

void LauncherPanel::PlaceNear(const RECT& bar, PanelDir dir) {
    auto& sc = Screen::Current();
    d_->dir = dir;
    float h = kPanelH;
    float winW = kBarW + 2 * kM;
    float winH = h + d_->gap + kM;
    int pw = sc.Px(winW), ph = sc.Px(winH);
    int left = (bar.left + bar.right) / 2 - pw / 2;
    RECT r;
    if (dir == PanelDir::Below) r = {left, bar.bottom, left + pw, bar.bottom + ph};
    else r = {left, bar.top - ph, left + pw, bar.top};
    Place(r);
    if (dir == PanelDir::Below) d_->pr = {kM, d_->gap, kM + kBarW, d_->gap + h};
    else d_->pr = {kM, kM, kM + kBarW, kM + h};
    d_->Rebuild();
}

void LauncherPanel::Appear(bool show) {
    if (show == shown_) return;
    shown_ = show;
    if (show) {
        d_->appear.Snap(gfx::ReducedMotion() ? 1.0f : 0.0f);
        d_->appear.To(1);
        Show(true);
    } else {
        d_->appear.To(0);
        d_->hot = -1;
        d_->dragging = false;
        d_->CommitRename(true);
        if (gfx::ReducedMotion()) Show(false);
    }
    RequestAnimationFrame();
}

bool LauncherPanel::Animate(float dt) {
    bool more = d_->appear.Step(dt);
    more |= d_->scroll.Step(dt);
    if (!shown_ && d_->appear.value <= 0.01f && !more) Show(false);
    return more;
}

void LauncherPanel::SetView(View v) {
    if (v != view_) {
        d_->scroll.Snap(0);
        d_->sel = -1;
    }
    view_ = v;
    d_->Rebuild();
    Invalidate();
}

void LauncherPanel::ResetToRoot() {
    d_->folderId.clear();
    d_->addMode = false;
    SetView(View::Favorites);
}

void LauncherPanel::Refresh() {
    d_->Rebuild();
    Invalidate();
}

// ============================================================================ building views

void LauncherPanel::Impl::HeaderButton(float& x, wchar_t glyph, const wchar_t* label, std::function<void()> click) {
    float w = label ? draw::TextWidth(label, 12) + 34 : 30;
    Hit h;
    h.scrolls = false;
    h.r = {x - w, pr.top + 8, x, pr.top + 36};
    std::wstring text = label ? label : L"";
    h.paint = [glyph, text](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
        if (hot || sel) draw::RoundRect(dc, r, 8, hot ? kSurfaceHover : kSurfaceRaised);
        if (sel) draw::FocusRing(dc, r, 8);
        if (text.empty()) draw::Glyph(dc, glyph, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, 14, kTextDim);
        else {
            draw::Glyph(dc, glyph, {r.left + 15, (r.top + r.bottom) / 2}, 12, kTextDim);
            draw::Text(dc, text, {r.left + 27, r.top, r.right - 6, r.bottom}, 12, kTextDim);
        }
    };
    h.click = std::move(click);
    h.selectable = true;
    hits.push_back(std::move(h));
    x -= w + 4;
}

void LauncherPanel::Impl::BuildHeader(const std::wstring& title, bool back, std::function<void()> onBack) {
    float x = pr.left + 10;
    if (back) {
        Hit b;
        b.scrolls = false;
        b.r = {x, pr.top + 8, x + 28, pr.top + 36};
        b.paint = [](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
            if (hot || sel) draw::RoundRect(dc, r, 8, kSurfaceHover);
            if (sel) draw::FocusRing(dc, r, 8);
            draw::Glyph(dc, g::Back, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, 13, kText);
        };
        b.click = std::move(onBack);
        b.selectable = true;
        hits.push_back(std::move(b));
        x += 34;
    }
    Hit t;
    t.scrolls = false;
    t.r = {x, pr.top + 8, pr.right - 120, pr.top + 36};
    t.paint = [title](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool) {
        draw::Text(dc, title, {r.left + 4, r.top, r.right, r.bottom}, 14, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    };
    hits.push_back(std::move(t));
}

void LauncherPanel::Impl::Launch(const std::wstring& id) {
    sys::AppCatalog::Launch(id);
    if (self->requestClose) self->requestClose(false);
}

void LauncherPanel::Impl::BuildGrid(const std::vector<FavItem>& items, bool inFolder) {
    float top = viewTop + 4;
    float cellW = (pr.right - pr.left - 16) / kCols;
    if (items.empty()) {
        Hit e;
        e.r = {pr.left + 20, top + 30, pr.right - 20, top + 90};
        std::wstring msg = inFolder ? L"Klasör boş. Uygulamaları sürükleyip bırakın ya da sağ tıklayıp \"Klasöre taşı\"yı seçin."
                                    : L"Henüz favori yok. \"Ekle\" ile uygulama ekleyin.";
        e.paint = [msg](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool) {
            draw::Text(dc, msg, r, 12.5f, kTextDim, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
        };
        hits.push_back(std::move(e));
        contentH = 120;
        return;
    }
    int px = static_cast<int>(std::ceil(34 * self->Scale()));
    int mini = static_cast<int>(std::ceil(16 * self->Scale()));
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        int col = static_cast<int>(i % kCols), row = static_cast<int>(i / kCols);
        Hit h;
        h.r = {pr.left + 8 + col * cellW, top + row * kTileH, pr.left + 8 + (col + 1) * cellW, top + (row + 1) * kTileH};
        h.selectable = true;
        h.drag = static_cast<int>(i);
        if (it.kind == FavItem::Kind::App) {
            std::wstring name = it.app.name, iconKey = L"app:" + it.app.id, id = it.app.id;
            h.paint = [name, iconKey, px](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
                D2D1_RECT_F bg{r.left + 3, r.top + 2, r.right - 3, r.bottom - 2};
                if (hot || sel) draw::RoundRect(dc, bg, 12, hot ? kSurfaceHover : kSurfaceRaised);
                if (sel) draw::FocusRing(dc, bg, 12);
                float cx = (r.left + r.right) / 2;
                D2D1_RECT_F ir{cx - 17, r.top + 10, cx + 17, r.top + 44};
                if (auto* b = gfx::IconCache::Get().Get(iconKey, px)) draw::Bitmap(dc, b, ir);
                else draw::RoundRect(dc, ir, 8, Rgba(255, 255, 255, 0.07f));
                draw::Text(dc, name, {r.left + 4, r.top + 50, r.right - 4, r.top + 68}, 11.5f, kText, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
            };
            h.click = [this, id] { Launch(id); };
        } else {
            std::wstring name = it.folderName, fid = it.folderId;
            std::vector<std::wstring> keys;
            for (size_t k = 0; k < it.children.size() && k < 4; ++k) keys.push_back(L"app:" + it.children[k].id);
            h.paint = [name, keys, mini](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
                D2D1_RECT_F bg{r.left + 3, r.top + 2, r.right - 3, r.bottom - 2};
                if (hot || sel) draw::RoundRect(dc, bg, 12, hot ? kSurfaceHover : kSurfaceRaised);
                if (sel) draw::FocusRing(dc, bg, 12);
                float cx = (r.left + r.right) / 2;
                D2D1_RECT_F box{cx - 19, r.top + 8, cx + 19, r.top + 46};
                draw::RoundRect(dc, box, 10, Rgba(255, 255, 255, 0.09f));
                for (size_t k = 0; k < keys.size(); ++k) {
                    float x = box.left + 3 + (k % 2) * 17, y = box.top + 3 + (k / 2) * 17;
                    if (auto* b = gfx::IconCache::Get().Get(keys[k], mini)) draw::Bitmap(dc, b, {x, y, x + 15, y + 15});
                }
                if (keys.empty()) draw::Glyph(dc, g::Folder, {cx, (box.top + box.bottom) / 2}, 16, kTextDim);
                draw::Text(dc, name, {r.left + 4, r.top + 50, r.right - 4, r.top + 68}, 11.5f, kText, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
            };
            h.click = [this, fid] {
                folderId = fid;
                self->SetView(View::Folder);
            };
        }
        h.menu = [this, i, inFolder](POINT at) { MenuForTile(static_cast<int>(i), inFolder, at); };
        hits.push_back(std::move(h));
    }
    size_t rows = (items.size() + kCols - 1) / kCols;
    contentH = rows * kTileH + 8;
}

void LauncherPanel::Impl::MenuForTile(int index, bool inFolder, POINT at) {
    auto& fav = Store::Get().favorites();
    std::vector<MenuItem> m;
    const FavItem* folder = inFolder ? fav.FindFolder(folderId) : nullptr;
    if (inFolder && !folder) return;
    size_t count = inFolder ? folder->children.size() : fav.Items().size();
    if (index < 0 || static_cast<size_t>(index) >= count) return;
    bool isFolderTile = !inFolder && fav.Items()[index].kind == FavItem::Kind::Folder;
    std::wstring appId = inFolder ? folder->children[index].id : (isFolderTile ? L"" : fav.Items()[index].app.id);
    std::wstring tileFolder = isFolderTile ? fav.Items()[index].folderId : L"";
    std::wstring inFolderId = folderId;

    if (!isFolderTile) m.push_back({L"Aç", g::Play, [this, appId] { Launch(appId); }});
    else m.push_back({L"Aç", g::Folder, [this, tileFolder] { folderId = tileFolder; self->SetView(View::Folder); }});
    auto move = [this, inFolder, inFolderId](int from, int to) {
        auto& f = Store::Get().favorites();
        if (inFolder) f.MoveInFolder(inFolderId, from, to);
        else f.MoveRoot(from, to);
        Changed();
    };
    MenuItem left{L"Sola taşı", g::ChevronLeft, [move, index] { move(index, index - 1); }};
    left.enabled = index > 0;
    left.separatorBefore = true;
    MenuItem right{L"Sağa taşı", g::ChevronRight, [move, index] { move(index, index + 1); }};
    right.enabled = static_cast<size_t>(index) + 1 < count;
    m.push_back(left);
    m.push_back(right);
    if (isFolderTile) {
        MenuItem ren{L"Yeniden adlandır", g::Rename, [this, tileFolder] { StartRename(tileFolder); }};
        ren.separatorBefore = true;
        m.push_back(ren);
        m.push_back({L"Klasörü sil (uygulamalar favorilerde kalır)", g::Delete, [this, tileFolder] {
                         Store::Get().favorites().DeleteFolder(tileFolder);
                         Changed();
                     }});
    } else {
        bool first = true;
        for (const auto& it : fav.Items()) {
            if (it.kind != FavItem::Kind::Folder || it.folderId == inFolderId) continue;
            std::wstring fid = it.folderId;
            MenuItem mi{L"Klasöre taşı: " + it.folderName, g::Folder, [this, appId, fid] {
                            Store::Get().favorites().MoveAppToFolder(appId, fid);
                            Changed();
                        }};
            mi.separatorBefore = first;
            first = false;
            m.push_back(mi);
        }
        MenuItem nf{L"Yeni klasöre taşı", g::Add, [this, appId] {
                        auto id = Store::Get().favorites().CreateFolder(L"Yeni klasör", {appId});
                        Changed();
                        StartRename(id);
                    }};
        nf.separatorBefore = first;
        m.push_back(nf);
        if (inFolder)
            m.push_back({L"Klasörden çıkar", g::Up, [this, appId] {
                             Store::Get().favorites().MoveAppToRoot(appId);
                             Changed();
                         }});
        MenuItem rm{L"Favorilerden kaldır", g::Delete, [this, appId] {
                        Store::Get().favorites().RemoveApp(appId);
                        Changed();
                    }};
        rm.separatorBefore = true;
        m.push_back(rm);
    }
    MenuPopup::Get().Show(std::move(m), at);
}

void LauncherPanel::Impl::BuildAllApps() {
    float top = viewTop + 2;
    const auto& catalogApps = sys::AppCatalog::Get().Apps();
    if (catalogApps.empty()) {
        Hit e;
        e.r = {pr.left + 20, top + 30, pr.right - 20, top + 70};
        e.paint = [](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool) {
            draw::Text(dc, L"Uygulamalar yükleniyor…", r, 12.5f, kTextDim, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
        };
        hits.push_back(std::move(e));
        contentH = 100;
        return;
    }
    int px = static_cast<int>(std::ceil(20 * self->Scale()));
    float rowH = 34;
    float y = top;
    for (const auto& a : catalogApps) {
        Hit h;
        h.r = {pr.left + 6, y, pr.right - 6, y + rowH};
        h.selectable = true;
        std::wstring id = a.id, name = a.name, key = a.IconKey();
        bool fav = Store::Get().favorites().Contains(id);
        bool add = addMode;
        h.paint = [name, key, px, fav](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
            if (hot || sel) draw::RoundRect(dc, r, 8, hot ? kSurfaceHover : kSurfaceRaised);
            if (sel) draw::FocusRing(dc, r, 8);
            float cy = (r.top + r.bottom) / 2;
            if (auto* b = gfx::IconCache::Get().Get(key, px)) draw::Bitmap(dc, b, {r.left + 10, cy - 10, r.left + 30, cy + 10});
            draw::Text(dc, name, {r.left + 40, r.top, r.right - 40, r.bottom}, 13, kText);
            draw::Glyph(dc, fav ? g::StarFill : g::Star, {r.right - 18, cy}, 13, fav ? kWarn : (hot ? kTextDim : kTextFaint));
        };
        h.click = [this, id, name, add] {
            POINT pt;
            GetCursorPos(&pt);
            PointF p = self->ScreenToDip(pt);
            bool onStar = p.x > pr.right - 46;
            if (add || onStar) {
                auto& f = Store::Get().favorites();
                if (f.Contains(id)) f.RemoveApp(id);
                else f.AddApp({id, name});
                Changed();
            } else {
                Launch(id);
            }
        };
        h.menu = [this, id, name](POINT at) {
            bool fav = Store::Get().favorites().Contains(id);
            std::vector<MenuItem> m;
            m.push_back({L"Aç", g::Play, [this, id] { Launch(id); }});
            m.push_back({fav ? L"Favorilerden kaldır" : L"Favorilere ekle", fav ? g::StarFill : g::Star, [this, id, name, fav] {
                             auto& f = Store::Get().favorites();
                             if (fav) f.RemoveApp(id);
                             else f.AddApp({id, name});
                             Changed();
                         }});
            MenuPopup::Get().Show(std::move(m), at);
        };
        hits.push_back(std::move(h));
        y += rowH;
    }
    contentH = y - top + 6;
}

void LauncherPanel::Impl::BuildSettings() {
    auto& st = Store::Get().settings();
    bool preview = paths::IsPreview();
    float y = viewTop + 2;
    auto row = [&](const std::wstring& label, const std::wstring& detail, std::function<void(ID2D1DeviceContext*, D2D1_RECT_F)> control,
                   std::function<void()> click, float h = 44) {
        Hit r;
        r.r = {pr.left + 6, y, pr.right - 6, y + h};
        r.selectable = static_cast<bool>(click);
        r.paint = [label, detail, control](ID2D1DeviceContext* dc, D2D1_RECT_F rr, bool hot, bool sel) {
            if (hot || sel) draw::RoundRect(dc, rr, 8, hot ? kSurfaceHover : kSurfaceRaised);
            if (sel) draw::FocusRing(dc, rr, 8);
            if (detail.empty()) draw::Text(dc, label, {rr.left + 12, rr.top, rr.right - 120, rr.bottom}, 13, kText);
            else {
                draw::Text(dc, label, {rr.left + 12, rr.top + 4, rr.right - 120, rr.top + 24}, 13, kText);
                draw::Text(dc, detail, {rr.left + 12, rr.top + 22, rr.right - 60, rr.bottom - 2}, 11, kTextDim);
            }
            if (control) control(dc, rr);
        };
        r.click = std::move(click);
        hits.push_back(std::move(r));
        y += h + 2;
    };
    auto toggle = [](bool on, bool enabled = true) {
        return [on, enabled](ID2D1DeviceContext* dc, D2D1_RECT_F r) { Toggle(dc, {r.right - 30, (r.top + r.bottom) / 2}, on, enabled); };
    };
    auto valueText = [](std::wstring v) {
        return [v](ID2D1DeviceContext* dc, D2D1_RECT_F r) {
            draw::Text(dc, v, {r.right - 150, r.top, r.right - 12, r.bottom}, 12.5f, kTextDim, DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing);
        };
    };

    auto method = preview ? sys::autostart::Method::None : sys::autostart::Current();
    row(L"Windows ile başlat",
        preview ? L"Önizleme modunda kapalı" : std::wstring(L"Yöntem: ") + sys::autostart::MethodName(method),
        toggle(method != sys::autostart::Method::None, !preview), [this, method, preview] {
            if (preview) return;
            std::wstring err;
            App::Get().SetAutostart(method == sys::autostart::Method::None, &err);
            Rebuild();
            self->Invalidate();
        });
    row(L"Windows görev çubuğunu gizle", preview ? L"Önizleme modunda gerçek görev çubuğuna dokunulmaz" : L"",
        toggle(st.hideTaskbar, !preview), [this] {
            App::Get().SetTaskbarHidden(!Store::Get().settings().hideTaskbar);
            Rebuild();
            self->Invalidate();
        });
    row(L"Windows görev çubuğunu geri getir", L"Gizlemeyi kapatır ve görev çubuğunu hemen gösterir",
        [](ID2D1DeviceContext* dc, D2D1_RECT_F r) { draw::Glyph(dc, g::Restore, {r.right - 24, (r.top + r.bottom) / 2}, 14, kTextDim); },
        [this] {
            App::Get().RestoreTaskbarNow();
            Rebuild();
            self->Invalidate();
        });
    std::wstring size = st.uiScale < 0.95 ? L"Küçük" : st.uiScale > 1.05 ? L"Büyük" : L"Normal";
    row(L"Arayüz boyutu", L"", valueText(size + L"  ›"), [this] {
        auto& s = Store::Get().settings();
        s.uiScale = s.uiScale < 0.95 ? 1.0 : s.uiScale < 1.05 ? 1.15 : 0.9;
        Store::Get().SaveSettings();
        ui::Post([] { App::Get().ApplySettings(); });
    });
    row(L"Animasyonları azalt", L"Windows'taki animasyon tercihi de dikkate alınır", toggle(st.reduceMotion), [this] {
        Store::Get().settings().reduceMotion = !Store::Get().settings().reduceMotion;
        Store::Get().SaveSettings();
        App::Get().ApplySettings();
        Rebuild();
        self->Invalidate();
    });
    row(L"Bildirim kutularını göster", L"Tam ekranda hiçbir zaman gösterilmez", toggle(st.showToasts), [this] {
        Store::Get().settings().showToasts = !Store::Get().settings().showToasts;
        Store::Get().SaveSettings();
        Rebuild();
        self->Invalidate();
    });
    auto acc = sys::NotificationMonitor::Get().Access();
    std::wstring accText = acc == sys::NotificationAccess::Allowed ? L"İzin verildi"
                           : acc == sys::NotificationAccess::Denied ? L"İzin verilmedi (Windows Gizlilik ayarları)"
                           : acc == sys::NotificationAccess::Unavailable ? L"Bu sistemde kullanılamıyor"
                                                                          : L"Açınca izin istenir";
    row(L"Windows bildirimlerini oku", accText, toggle(st.windowsNotifications), [this] {
        App::Get().EnableWindowsNotifications(!Store::Get().settings().windowsNotifications);
        Rebuild();
        self->Invalidate();
    });
    row(L"Windows tuşu arama çubuğunu açsın", L"Win+E, Win+D gibi kısayollar etkilenmez", toggle(st.captureWinKey), [this] {
        Store::Get().settings().captureWinKey = !Store::Get().settings().captureWinKey;
        Store::Get().SaveSettings();
        App::Get().ApplySettings();
        Rebuild();
        self->Invalidate();
    });
    row(L"Arama motoru", L"", valueText(std::wstring(web::FindEngine(st.searchEngine).name) + L"  ›"), [this] {
        auto& s = Store::Get().settings();
        const auto& list = web::Engines();
        size_t i = 0;
        for (; i < list.size(); ++i)
            if (s.searchEngine == list[i].id) break;
        s.searchEngine = list[(i + 1) % list.size()].id;
        Store::Get().SaveSettings();
        Rebuild();
        self->Invalidate();
    });
    std::wstring prov;
    for (const auto& p : sys::BatteryService::Get().Providers()) {
        if (!prov.empty()) prov += L" · ";
        prov += p.name + L": " + p.status;
    }
    row(L"Cihaz pil desteği", prov.empty() ? L"Kapalı" : Ellipsize(prov, 70), toggle(st.deviceBattery), [this] {
        auto& s = Store::Get().settings();
        s.deviceBattery = !s.deviceBattery;
        Store::Get().SaveSettings();
        if (s.deviceBattery) sys::BatteryService::Get().Start();
        else sys::BatteryService::Get().Stop();
        Rebuild();
        self->Invalidate();
    });
    row(L"NotchBar'dan çık", L"Görev çubuğu geri getirilir",
        [](ID2D1DeviceContext* dc, D2D1_RECT_F r) { draw::Glyph(dc, g::Power, {r.right - 24, (r.top + r.bottom) / 2}, 14, kBad); },
        [] { ui::Post([] { App::Get().Quit(); }); });
    contentH = y - viewTop + 6;
}

void LauncherPanel::Impl::BuildResults() {
    float y = viewTop;
    auto section = [&](const wchar_t* label) {
        Hit s;
        s.r = {pr.left + 14, y, pr.right - 14, y + 24};
        std::wstring l = label;
        s.paint = [l](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool) {
            draw::Text(dc, l, r, 11, kTextFaint, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        };
        hits.push_back(std::move(s));
        y += 24;
    };
    // Calculation
    if (calcRes.status != calc::Status::NotExpression) {
        Hit c;
        c.r = {pr.left + 6, y + 4, pr.right - 6, y + 56};
        bool ok = calcRes.status == calc::Status::Ok;
        std::wstring value = ok ? L"= " + calc::Format(calcRes.value, DecimalSep()) : calc::StatusMessage(calcRes.status);
        bool cp = copied;
        c.selectable = ok;
        c.paint = [value, ok, cp](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
            if (hot || sel) draw::RoundRect(dc, r, 10, hot ? kSurfaceHover : kSurfaceRaised);
            if (sel) draw::FocusRing(dc, r, 10);
            draw::Glyph(dc, g::Calculator, {r.left + 22, (r.top + r.bottom) / 2}, 16, ok ? kText : kWarn);
            draw::Text(dc, value, {r.left + 46, r.top, r.right - 110, r.bottom}, ok ? 20.0f : 13.0f, ok ? kText : kWarn,
                       ok ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
            if (ok)
                draw::Text(dc, cp ? L"Kopyalandı ✓" : L"Enter: kopyala", {r.right - 120, r.top, r.right - 12, r.bottom}, 11,
                           cp ? kGood : kTextFaint, DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing);
        };
        if (ok) {
            std::wstring raw = calc::Format(calcRes.value, DecimalSep());
            c.click = [this, raw] {
                CopyToClipboard(self->Hwnd(), raw);
                copied = true;
                Rebuild();
                self->Invalidate();
            };
        }
        hits.push_back(std::move(c));
        y += 62;
    }
    int px = static_cast<int>(std::ceil(22 * self->Scale()));
    if (!apps.empty()) {
        section(L"UYGULAMALAR");
        for (const auto& a : apps) {
            Hit h;
            h.r = {pr.left + 6, y, pr.right - 6, y + 38};
            h.selectable = true;
            std::wstring id = a.id, name = a.name, key = a.IconKey();
            h.paint = [name, key, px](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
                if (hot || sel) draw::RoundRect(dc, r, 8, hot ? kSurfaceHover : kSurfaceRaised);
                if (sel) draw::FocusRing(dc, r, 8);
                float cy = (r.top + r.bottom) / 2;
                if (auto* b = gfx::IconCache::Get().Get(key, px)) draw::Bitmap(dc, b, {r.left + 10, cy - 11, r.left + 32, cy + 11});
                draw::Text(dc, name, {r.left + 44, r.top, r.right - 12, r.bottom}, 13.5f, kText);
            };
            h.click = [this, id] { Launch(id); };
            h.menu = [this, id, name](POINT at) {
                bool fav = Store::Get().favorites().Contains(id);
                std::vector<MenuItem> m;
                m.push_back({L"Aç", g::Play, [this, id] { Launch(id); }});
                m.push_back({fav ? L"Favorilerden kaldır" : L"Favorilere ekle", fav ? g::StarFill : g::Star, [this, id, name, fav] {
                                 auto& f = Store::Get().favorites();
                                 if (fav) f.RemoveApp(id);
                                 else f.AddApp({id, name});
                                 Changed();
                             }});
                MenuPopup::Get().Show(std::move(m), at);
            };
            hits.push_back(std::move(h));
            y += 38;
        }
    }
    if (!files.empty() || filesPending || fileSrc == sys::FileSource::Fallback) {
        section(fileSrc == sys::FileSource::Fallback ? L"DOSYALAR (Windows Search kapalı: yalnızca Masaüstü/Belgeler/İndirilenler)"
                                                     : L"DOSYALAR");
        if (files.empty()) {
            Hit p;
            p.r = {pr.left + 14, y, pr.right - 14, y + 30};
            bool pending = filesPending;
            p.paint = [pending](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool) {
                draw::Text(dc, pending ? L"Aranıyor…" : L"Eşleşen dosya yok", r, 12, kTextDim);
            };
            hits.push_back(std::move(p));
            y += 30;
        }
        for (const auto& f : files) {
            Hit h;
            h.r = {pr.left + 6, y, pr.right - 6, y + 42};
            h.selectable = true;
            sys::FileHit fh = f;
            std::wstring key = L"file:" + f.path;
            h.paint = [fh, key, px](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
                if (hot || sel) draw::RoundRect(dc, r, 8, hot ? kSurfaceHover : kSurfaceRaised);
                if (sel) draw::FocusRing(dc, r, 8);
                float cy = (r.top + r.bottom) / 2;
                if (auto* b = gfx::IconCache::Get().Get(key, px)) draw::Bitmap(dc, b, {r.left + 10, cy - 11, r.left + 32, cy + 11});
                else draw::Glyph(dc, fh.isFolder ? g::Folder : g::Document, {r.left + 21, cy}, 15, kTextDim);
                draw::Text(dc, fh.name, {r.left + 44, r.top + 3, r.right - 12, r.top + 23}, 13, kText);
                draw::Text(dc, fh.folder, {r.left + 44, r.top + 21, r.right - 12, r.bottom - 3}, 10.5f, kTextFaint);
            };
            h.click = [this, fh] {
                sys::FileSearch::Open(fh);
                if (self->requestClose) self->requestClose(false);
            };
            h.menu = [this, fh](POINT at) {
                std::vector<MenuItem> m;
                m.push_back({L"Aç", g::Play, [this, fh] {
                                 sys::FileSearch::Open(fh);
                                 if (self->requestClose) self->requestClose(false);
                             }});
                m.push_back({L"Klasörde göster", g::Folder, [this, fh] {
                                 sys::FileSearch::ShowInFolder(fh);
                                 if (self->requestClose) self->requestClose(false);
                             }});
                m.push_back({L"Yolu kopyala", g::Copy, [this, fh] { CopyToClipboard(self->Hwnd(), fh.path); }});
                MenuPopup::Get().Show(std::move(m), at);
            };
            hits.push_back(std::move(h));
            y += 42;
        }
    }
    section(L"WEB");
    {
        Hit w;
        w.r = {pr.left + 6, y, pr.right - 6, y + 38};
        w.selectable = true;
        std::wstring engine = web::FindEngine(Store::Get().settings().searchEngine).name;
        std::wstring q = query;
        w.paint = [q, engine](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool sel) {
            if (hot || sel) draw::RoundRect(dc, r, 8, hot ? kSurfaceHover : kSurfaceRaised);
            if (sel) draw::FocusRing(dc, r, 8);
            draw::Glyph(dc, g::Globe, {r.left + 21, (r.top + r.bottom) / 2}, 15, kTextDim);
            draw::Text(dc, L"“" + Ellipsize(q, 40) + L"” için " + engine + L"'da ara", {r.left + 44, r.top, r.right - 12, r.bottom},
                       13, kText);
        };
        w.click = [this, q] {
            std::wstring url = web::BuildSearchUrl(Store::Get().settings().searchEngine, q);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); // default browser
            if (self->requestClose) self->requestClose(false);
        };
        hits.push_back(std::move(w));
        y += 38;
    }
    contentH = y - viewTop + 6;
}

void LauncherPanel::Impl::Rebuild() {
    int oldSel = sel;
    hits.clear();
    hot = -1;
    auto v = self->view_;
    bool header = v != View::Results;
    viewTop = header ? pr.top + kHeaderH : pr.top + 6;
    viewBottom = pr.bottom - 6;
    if (v == View::Favorites) {
        BuildHeader(L"Favoriler", false, nullptr);
        float x = pr.right - 10;
        HeaderButton(x, g::Settings, nullptr, [this] { self->SetView(View::Settings); });
        HeaderButton(x, g::AllApps, L"Tüm uygulamalar", [this] {
            addMode = false;
            self->SetView(View::AllApps);
        });
        HeaderButton(x, g::Add, L"Ekle", [this] {
            addMode = true;
            self->SetView(View::AllApps);
        });
        BuildGrid(Store::Get().favorites().Items(), false);
    } else if (v == View::Folder) {
        const FavItem* f = Store::Get().favorites().FindFolder(folderId);
        if (!f) {
            self->view_ = View::Favorites;
            Rebuild();
            return;
        }
        BuildHeader(f->folderName, true, [this] { self->SetView(View::Favorites); });
        float x = pr.right - 10;
        std::wstring fid = folderId;
        HeaderButton(x, g::Rename, L"Yeniden adlandır", [this, fid] { StartRename(fid); });
        std::vector<FavItem> kids;
        for (const auto& c : f->children) {
            FavItem fi;
            fi.app = c;
            kids.push_back(fi);
        }
        BuildGrid(kids, true);
    } else if (v == View::AllApps) {
        BuildHeader(addMode ? L"Favorilere ekle" : L"Tüm uygulamalar", true, [this] { self->SetView(View::Favorites); });
        BuildAllApps();
    } else if (v == View::Settings) {
        BuildHeader(L"Ayarlar", true, [this] { self->SetView(View::Favorites); });
        BuildSettings();
    } else {
        BuildResults();
    }
    float maxScroll = std::max(0.0f, contentH - (viewBottom - viewTop));
    if (scroll.target > maxScroll) scroll.To(maxScroll);
    auto selectable = Selectable();
    if (keyboard || v == View::Results) {
        if (oldSel < 0 || oldSel >= static_cast<int>(hits.size()) || !hits[oldSel].selectable) {
            // Results: first scrolling item is the default (Enter opens it). Others: none until keys are used.
            sel = -1;
            for (int i : selectable)
                if (hits[i].scrolls) {
                    sel = i;
                    break;
                }
        } else sel = oldSel;
    } else sel = -1;
}

// ============================================================================ query

void LauncherPanel::SetQuery(const std::wstring& q) {
    auto& d = *d_;
    std::wstring t = Trim(q);
    d.query = t;
    d.copied = false;
    d.scroll.Snap(0);
    d.sel = -1;
    ClearTimeout(d.fileTimer);
    if (t.empty()) {
        gate_.CancelAll();
        d.apps.clear();
        d.files.clear();
        d.filesPending = false;
        return;
    }
    d.calcRes = calc::Evaluate(t);
    // Apps: in-memory ranking, instant.
    auto folded = search::Fold(t);
    std::vector<std::pair<int, const sys::AppEntry*>> ranked;
    for (const auto& a : sys::AppCatalog::Get().Apps())
        if (int r = search::RankName(a.folded, folded)) ranked.push_back({r, &a});
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    d.apps.clear();
    for (size_t i = 0; i < ranked.size() && i < 6; ++i) d.apps.push_back(*ranked[i].second);
    // Files: debounced, cancellable; stale results are dropped by the generation check.
    auto ticket = gate_.Begin();
    d.files.clear();
    bool isCalc = d.calcRes.status != calc::Status::NotExpression;
    d.filesPending = !isCalc && t.size() >= 2;
    if (d.filesPending) {
        d.fileTimer = SetTimeout(160, [this, t, ticket] {
            d_->fileTimer = 0;
            sys::FileSearch::Get().Query(t, ticket, [this, ticket](std::vector<sys::FileHit> hits, sys::FileSource src) {
                if (!gate_.IsCurrent(ticket)) return; // an older query: ignore
                d_->files = std::move(hits);
                d_->fileSrc = src;
                d_->filesPending = false;
                d_->Rebuild();
                Invalidate();
            });
        });
    }
    SetView(View::Results);
}

// ============================================================================ input

int LauncherPanel::Impl::HitAt(PointF p) const {
    for (int i = static_cast<int>(hits.size()) - 1; i >= 0; --i) {
        const auto& h = hits[i];
        if (!h.click && !h.menu) continue;
        auto r = Screen(h);
        if (h.scrolls && (p.y < viewTop || p.y >= viewBottom)) continue;
        if (PointIn(r, p)) return i;
    }
    return -1;
}

void LauncherPanel::Impl::EnsureVisible(int i) {
    if (i < 0) return;
    const auto& h = hits[i];
    if (!h.scrolls) return;
    float top = h.r.top - scroll.target, bottom = h.r.bottom - scroll.target;
    float maxScroll = std::max(0.0f, contentH - (viewBottom - viewTop));
    if (top < viewTop) scroll.To(std::clamp(scroll.target - (viewTop - top) - 4, 0.0f, maxScroll));
    else if (bottom > viewBottom) scroll.To(std::clamp(scroll.target + (bottom - viewBottom) + 4, 0.0f, maxScroll));
}

bool LauncherPanel::HandleKey(UINT vk) {
    auto& d = *d_;
    auto sel = d.Selectable();
    if (vk == ('C' | 0x10000)) { // Ctrl+C without text selection: copy the calculation
        if (view_ == View::Results && d.calcRes.status == calc::Status::Ok) {
            CopyToClipboard(Hwnd(), calc::Format(d.calcRes.value, DecimalSep()));
            d.copied = true;
            d.Rebuild();
            Invalidate();
            return true;
        }
        return false;
    }
    if (vk == VK_ESCAPE) {
        if (view_ == View::Folder || view_ == View::AllApps || view_ == View::Settings) {
            SetView(View::Favorites);
            return true;
        }
        return false;
    }
    if (sel.empty()) return false;
    bool grid = view_ == View::Favorites || view_ == View::Folder;
    int step = 0;
    if (vk == VK_DOWN) step = grid ? kCols : 1;
    else if (vk == VK_UP) step = grid ? -kCols : -1;
    else if (vk == VK_NEXT) step = 1;   // PageDown: next tile in grids
    else if (vk == VK_PRIOR) step = -1; // PageUp: previous tile
    else if (vk == VK_TAB) step = (GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
    if (step) {
        d.keyboard = true;
        auto pos = std::find(sel.begin(), sel.end(), d.sel);
        int idx = pos == sel.end() ? -1 : static_cast<int>(pos - sel.begin());
        if (idx < 0) {
            // Start at the first content item, not the header buttons.
            idx = 0;
            for (size_t k = 0; k < sel.size(); ++k)
                if (d.hits[sel[k]].scrolls) {
                    idx = static_cast<int>(k);
                    break;
                }
        } else {
            idx = std::clamp(idx + step, 0, static_cast<int>(sel.size()) - 1);
        }
        d.sel = sel[idx];
        d.EnsureVisible(d.sel);
        RequestAnimationFrame();
        return true;
    }
    if (vk == VK_RETURN) {
        int target = d.sel;
        if (target < 0 && view_ == View::Results) target = sel.front();
        if (target >= 0 && d.hits[target].click) {
            auto fn = d.hits[target].click;
            fn();
            return true;
        }
    }
    return false;
}

void LauncherPanel::OnMouseMove(PointF p) {
    auto& d = *d_;
    if (onPointerEnter) onPointerEnter();
    if (d.downHit >= 0 && GetKeyState(VK_LBUTTON) < 0) {
        float dx = p.x - d.downPt.x, dy = p.y - d.downPt.y;
        if (!d.dragging && d.hits[d.downHit].drag >= 0 && dx * dx + dy * dy > 36 &&
            (view_ == View::Favorites || view_ == View::Folder)) {
            d.dragging = true;
            d.dragIdx = d.hits[d.downHit].drag;
            Tooltip::Get().Hide();
        }
        if (d.dragging) {
            d.dragPt = p;
            d.dropInsert = d.dropMerge = -1;
            for (const auto& h : d.hits) {
                if (h.drag < 0) continue;
                auto r = d.Screen(h);
                if (!PointIn(r, p)) continue;
                float w = r.right - r.left;
                bool center = p.x > r.left + w * 0.25f && p.x < r.right - w * 0.25f;
                if (center && h.drag != d.dragIdx && view_ == View::Favorites) d.dropMerge = h.drag;
                else d.dropInsert = (p.x < (r.left + r.right) / 2) ? h.drag : h.drag + 1;
            }
            Invalidate();
            return;
        }
    }
    int h = d.HitAt(p);
    if (h != d.hot) {
        d.hot = h;
        d.keyboard = false;
        Invalidate();
    }
}

void LauncherPanel::OnMouseLeave() {
    d_->hot = -1;
    Invalidate();
    if (onPointerLeave) onPointerLeave();
}

void LauncherPanel::OnMouseDown(PointF p, int button) {
    auto& d = *d_;
    if (MenuPopup::Get().IsOpen()) MenuPopup::Get().Close(true);
    d.downHit = d.HitAt(p);
    d.downPt = p;
    d.dragging = false;
}

void LauncherPanel::Impl::FinishDrop() {
    auto& fav = Store::Get().favorites();
    bool inFolder = self->view_ == View::Folder;
    if (inFolder) {
        const FavItem* f = fav.FindFolder(folderId);
        if (f && dropInsert >= 0) {
            int to = dropInsert > dragIdx ? dropInsert - 1 : dropInsert;
            to = std::clamp(to, 0, static_cast<int>(f->children.size()) - 1);
            fav.MoveInFolder(folderId, dragIdx, to);
            Changed();
        }
        return;
    }
    const auto& items = fav.Items();
    if (dragIdx < 0 || dragIdx >= static_cast<int>(items.size())) return;
    if (dropMerge >= 0 && dropMerge < static_cast<int>(items.size())) {
        const auto& src = items[dragIdx];
        const auto& dst = items[dropMerge];
        if (src.kind != FavItem::Kind::App) return; // folders are single level in v1
        std::wstring srcId = src.app.id;
        if (dst.kind == FavItem::Kind::Folder) {
            fav.MoveAppToFolder(srcId, dst.folderId);
            Changed();
        } else {
            auto id = fav.CreateFolder(L"Yeni klasör", {dst.app.id, srcId});
            Changed();
            StartRename(id);
        }
    } else if (dropInsert >= 0) {
        int to = dropInsert > dragIdx ? dropInsert - 1 : dropInsert;
        to = std::clamp(to, 0, static_cast<int>(items.size()) - 1);
        fav.MoveRoot(dragIdx, to);
        Changed();
    }
}

void LauncherPanel::OnMouseUp(PointF p, int button) {
    auto& d = *d_;
    if (d.dragging && button == 0) {
        d.dragging = false;
        d.FinishDrop();
        d.downHit = -1;
        d.Rebuild();
        Invalidate();
        return;
    }
    int h = d.HitAt(p);
    bool same = h >= 0 && h == d.downHit;
    d.downHit = -1;
    if (!same && button == 0) return;
    if (h < 0) return;
    if (button == 0 && d.hits[h].click) {
        auto fn = d.hits[h].click; // copy: the handler may rebuild hits
        fn();
    } else if (button == 1 && d.hits[h].menu) {
        POINT pt;
        GetCursorPos(&pt);
        auto fn = d.hits[h].menu;
        fn(pt);
    }
}

void LauncherPanel::OnWheel(PointF, int delta) {
    auto& d = *d_;
    float maxScroll = std::max(0.0f, d.contentH - (d.viewBottom - d.viewTop));
    d.scroll.To(std::clamp(d.scroll.target - delta / 120.0f * 60.0f, 0.0f, maxScroll));
    RequestAnimationFrame();
}

// ============================================================================ rename

void LauncherPanel::Impl::StartRename(const std::wstring& fid) {
    const FavItem* f = Store::Get().favorites().FindFolder(fid);
    if (!f) return;
    if (self->view_ != View::Folder || folderId != fid) {
        folderId = fid;
        self->SetView(View::Folder);
    }
    renamingFolder = fid;
    if (!rename) {
        rename = std::make_unique<TextInput>();
        TextInput::Callbacks cb;
        cb.onKey = [this](UINT vk) {
            if (vk == VK_RETURN) {
                CommitRename(true);
                return true;
            }
            if (vk == VK_ESCAPE) {
                CommitRename(false);
                return true;
            }
            return false;
        };
        cb.onFocusLost = [this](HWND) { ui::Post([this] { CommitRename(true); }); };
        rename->Create(self->Hwnd(), L"Klasör adı", cb);
    }
    float x = pr.left + 44;
    POINT tl = self->DipToScreen({x, pr.top + 10});
    POINT br = self->DipToScreen({pr.right - 150, pr.top + 34});
    rename->Place({tl.x, tl.y, br.x, br.y}, 14 * self->Scale(), RGB(36, 37, 41));
    rename->SetText(f->folderName, true);
    rename->Show(true, true);
}

void LauncherPanel::Impl::CommitRename(bool commit) {
    if (renamingFolder.empty() || !rename) return;
    std::wstring fid = renamingFolder;
    renamingFolder.clear();
    std::wstring name = rename->Text();
    rename->Show(false, false);
    if (commit && Store::Get().favorites().RenameFolder(fid, name)) Changed();
    if (self->refocusSearch) self->refocusSearch();
    Rebuild();
    self->Invalidate();
}

// ============================================================================ paint

void LauncherPanel::Paint(ID2D1DeviceContext* dc) {
    auto& d = *d_;
    float a = std::clamp(d.appear.value, 0.0f, 1.0f);
    if (a <= 0.001f) return;
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    float slide = (1 - a) * (d.dir == PanelDir::Below ? -8.0f : 8.0f);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(0, slide) * base);
    draw::Panel(dc, d.pr, kPanelRadius, a);
    bool layer = a < 0.999f;
    if (layer)
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a),
                      nullptr);
    // fixed header
    for (int i = 0; i < static_cast<int>(d.hits.size()); ++i) {
        const auto& h = d.hits[i];
        if (h.scrolls) continue;
        if (h.paint) h.paint(dc, h.r, i == d.hot, i == d.sel);
    }
    if (CurrentView() != View::Results)
        dc->DrawLine({d.pr.left + 12, d.viewTop - 1}, {d.pr.right - 12, d.viewTop - 1}, draw::Brush(dc, kEdge), 1);
    // scrolling content
    dc->PushAxisAlignedClip({d.pr.left, d.viewTop, d.pr.right, d.viewBottom}, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (int i = 0; i < static_cast<int>(d.hits.size()); ++i) {
        const auto& h = d.hits[i];
        if (!h.scrolls) continue;
        auto r = d.Screen(h);
        if (r.bottom < d.viewTop || r.top > d.viewBottom) continue; // virtualized: off-screen rows are not drawn
        bool dragged = d.dragging && h.drag == d.dragIdx;
        if (dragged) continue;
        if (h.paint) h.paint(dc, r, i == d.hot && !d.dragging, i == d.sel);
        if (d.dragging && h.drag >= 0) {
            if (h.drag == d.dropMerge) dc->DrawRoundedRectangle({{r.left + 3, r.top + 2, r.right - 3, r.bottom - 2}, 12, 12}, draw::Brush(dc, kAccent), 2);
            if (h.drag == d.dropInsert) dc->DrawLine({r.left, r.top + 10}, {r.left, r.bottom - 14}, draw::Brush(dc, kAccent), 2.5f);
            if (h.drag + 1 == d.dropInsert && (h.drag + 1) % kCols == 0)
                dc->DrawLine({r.right, r.top + 10}, {r.right, r.bottom - 14}, draw::Brush(dc, kAccent), 2.5f);
        }
    }
    // scrollbar hint
    float viewH = d.viewBottom - d.viewTop;
    if (d.contentH > viewH + 1) {
        float frac = viewH / d.contentH;
        float barH = std::max(24.0f, viewH * frac);
        float t = d.scroll.value / (d.contentH - viewH);
        float y = d.viewTop + (viewH - barH) * t;
        draw::RoundRect(dc, {d.pr.right - 5, y, d.pr.right - 2, y + barH}, 1.5f, Rgba(255, 255, 255, 0.18f));
    }
    dc->PopAxisAlignedClip();
    // dragged tile follows the pointer
    if (d.dragging) {
        for (const auto& h : d.hits)
            if (h.drag == d.dragIdx && h.paint) {
                float w = h.r.right - h.r.left, hh = h.r.bottom - h.r.top;
                D2D1_RECT_F r{d.dragPt.x - w / 2, d.dragPt.y - hh / 2, d.dragPt.x + w / 2, d.dragPt.y + hh / 2};
                dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), 0.85f),
                              nullptr);
                h.paint(dc, r, true, false);
                dc->PopLayer();
            }
    }
    if (layer) dc->PopLayer();
    dc->SetTransform(base);
}

} // namespace nb::ui
