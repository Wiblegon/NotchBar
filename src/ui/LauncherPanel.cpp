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
#include "ui/Launcher.h"
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
    std::wstring hint;                    // settings tiles: explanation shown in the hint line while hovered
    bool noDefault = false;               // never the automatic selection (e.g. the web row)
    std::function<void(PointF)> slide;    // slider: pressed / dragged (window DIP position)
    std::function<void()> slideEnd;       // slider: released
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
    gfx::HoverBank anim;       // hover / press / on-off amounts of the settings controls (by hit index)
    float pendingScale = 1.0f; // settings slider value while dragging
    int sliderHit = -1;
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
    float h = settingsOnly ? kSettingsH : kPanelH;
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
    more |= d_->anim.Step(dt);
    if (!shown_ && d_->appear.value <= 0.01f && !more) Show(false);
    return more;
}

void LauncherPanel::SetView(View v) {
    if (v != view_) {
        d_->anim.Clear();
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
    pendingScale = static_cast<float>(st.uiScale);
    float y = viewTop + 4;
    const float left = pr.left + 12, right = pr.right - 12;
    constexpr int cols = 3;
    constexpr float gapT = 8, tileHt = 66;
    const float tileW = (right - left - gapT * (cols - 1)) / cols;

    auto section = [&](const wchar_t* label) {
        Hit s;
        s.r = {left, y, right, y + 26};
        std::wstring l = label;
        s.paint = [l](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool) {
            draw::Text(dc, l, {r.left + 2, r.top + 4, r.right, r.bottom}, 11, kTextFaint, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        };
        hits.push_back(std::move(s));
        y += 26;
    };

    // Tiles in the style of the quick settings box: glyph, label, on/off state; the explanation of
    // the tile under the pointer shows in the hint line under the grid.
    int col = 0;
    float rowTop = y;
    auto tile = [&](wchar_t glyph, std::wstring label, std::wstring hint, int state /* -1 none, 0 off, 1 on */, bool enabled,
                    bool danger, std::function<void()> click) {
        Hit t;
        t.r = {left + col * (tileW + gapT), rowTop, left + col * (tileW + gapT) + tileW, rowTop + tileHt};
        t.selectable = true;
        t.hint = std::move(hint);
        int idx = static_cast<int>(hits.size());
        anim.To(gfx::HoverBank::State, idx, state == 1 ? 1.0f : 0.0f); // animates when the setting flips
        t.paint = [=, this](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool sel) {
            float hv = anim.Get(gfx::HoverBank::Hover, idx), pr = anim.Get(gfx::HoverBank::Press, idx);
            float on = std::clamp(anim.Get(gfx::HoverBank::State, idx), 0.0f, 1.0f);
            D2D1_MATRIX_3X2_F base;
            dc->GetTransform(&base);
            float s = 1.0f + 0.02f * hv - 0.05f * pr;
            dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}) * base);
            float offA = 0.05f + 0.05f * hv, onA = 0.26f + 0.08f * hv;
            draw::RoundRect(dc, r, 10,
                            D2D1::ColorF((255 + (120 - 255) * on) / 255.f, (255 + (170 - 255) * on) / 255.f, 1.0f, offA + (onA - offA) * on));
            auto c = !enabled ? kTextFaint : danger ? kBad : state == 1 ? kAccent : kText;
            draw::Glyph(dc, glyph, {(r.left + r.right) / 2, r.top + 23 - 1.5f * hv}, 17 + hv, c);
            draw::Text(dc, label, {r.left + 4, r.top + 38, r.right - 4, r.bottom - 2}, 11, !enabled ? kTextFaint : kText,
                       DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
            if (state >= 0 && enabled)
                draw::Text(dc, state == 1 ? L"Açık" : L"Kapalı", {r.left + 4, r.top + 4, r.right - 8, r.top + 18}, 9.5f,
                           state == 1 ? kAccent : kTextFaint, DWRITE_FONT_WEIGHT_NORMAL, Align::Trailing);
            if (sel) draw::FocusRing(dc, r, 10);
            dc->SetTransform(base);
        };
        if (enabled) t.click = std::move(click);
        else t.click = [] {};
        hits.push_back(std::move(t));
        if (++col == cols) {
            col = 0;
            rowTop += tileHt + gapT;
        }
    };
    auto endTiles = [&] {
        if (col != 0) {
            col = 0;
            rowTop += tileHt + gapT;
        }
        y = rowTop;
    };
    auto redo = [this] {
        Rebuild();
        self->Invalidate();
    };
    auto setBool = [this, redo](bool Settings::*field, bool applyNow) {
        return [this, redo, field, applyNow] {
            auto& s = Store::Get().settings();
            s.*field = !(s.*field);
            Store::Get().SaveSettings();
            if (applyNow) App::Get().ApplySettings();
            redo();
        };
    };

    // ---- behaviour
    section(L"Davranış");
    rowTop = y;
    auto method = preview ? sys::autostart::Method::None : sys::autostart::Current();
    tile(g::Play, L"Başlangıçta aç", preview ? L"Önizleme modunda kapalı" : std::wstring(L"Windows açılınca NotchBar'ı başlatır. Yöntem: ") + sys::autostart::MethodName(method),
         method != sys::autostart::Method::None, !preview, false, [this, method, redo] {
             std::wstring err;
             App::Get().SetAutostart(method == sys::autostart::Method::None, &err);
             redo();
         });
    tile(g::Laptop, L"Görev çubuğunu gizle", preview ? L"Önizleme modunda gerçek görev çubuğuna dokunulmaz" : L"Windows'un kendi görev çubuğunu gizler; ekran alanı pencerelere kalır.",
         st.hideTaskbar, !preview, false, [this, redo] {
             App::Get().SetTaskbarHidden(!Store::Get().settings().hideTaskbar);
             redo();
         });
    tile(g::Timer, L"Mini saat modu", L"Büyütülmüş pencerede üstte yalnızca küçük bir saat görünür; üzerine gelince çentik açılır.",
         st.maximizedClockOnly, true, false, setBool(&Settings::maximizedClockOnly, true));
    tile(g::Star, L"Az animasyon", L"Hareketleri azaltır. Windows'taki animasyon tercihi de dikkate alınır.", st.reduceMotion, true, false,
         setBool(&Settings::reduceMotion, true));
    tile(g::Bell, L"Bildirim kutuları", L"Gelen bildirimleri çentiğin altında kısa süre gösterir. Tam ekranda hiç gösterilmez.",
         st.showToasts, true, false, setBool(&Settings::showToasts, false));
    auto acc = sys::NotificationMonitor::Get().Access();
    std::wstring accText = acc == sys::NotificationAccess::Allowed ? L"Erişim izni verildi."
                           : acc == sys::NotificationAccess::Denied ? L"İzin verilmedi: Windows Gizlilik ayarlarından açın."
                           : acc == sys::NotificationAccess::Unavailable ? L"Bu sistemde kullanılamıyor."
                                                                          : L"Açınca izin istenir.";
    tile(g::Document, L"Windows bildirimleri", L"Windows bildirimlerini okuyup listeler. " + accText, st.windowsNotifications, true, false,
         [this, redo] {
             App::Get().EnableWindowsNotifications(!Store::Get().settings().windowsNotifications);
             redo();
         });
    tile(g::Keyboard, L"Win tuşu = arama", L"Windows tuşu arama çubuğunu açar. Win+E, Win+D gibi kısayollar etkilenmez.", st.captureWinKey, true,
         false, setBool(&Settings::captureWinKey, true));
    std::wstring prov;
    for (const auto& p : sys::BatteryService::Get().Providers()) {
        if (!prov.empty()) prov += L" · ";
        prov += p.name + L": " + p.status;
    }
    tile(g::Headset, L"Cihaz pilleri", prov.empty() ? L"Kulaklık, fare gibi kablosuz cihazların pilini gösterir." : Ellipsize(prov, 90),
         st.deviceBattery, true, false, [this, redo] {
             auto& s = Store::Get().settings();
             s.deviceBattery = !s.deviceBattery;
             Store::Get().SaveSettings();
             if (s.deviceBattery) sys::BatteryService::Get().Start();
             else sys::BatteryService::Get().Stop();
             redo();
         });
    endTiles();

    // hint line: explanation of the tile under the pointer
    {
        Hit h;
        h.r = {left, y - 2, right, y + 34};
        h.paint = [this](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool) {
            std::wstring t = L"Bir kutunun üzerine gelince açıklaması burada görünür.";
            if (hot >= 0 && hot < static_cast<int>(hits.size()) && !hits[hot].hint.empty()) t = hits[hot].hint;
            draw::Text(dc, t, {r.left + 4, r.top + 2, r.right - 4, r.bottom}, 11, kTextDim);
        };
        hits.push_back(std::move(h));
        y += 38;
    }

    // ---- size: slider, applied when released
    section(L"Arayüz boyutu");
    {
        Hit s;
        s.r = {left, y, right, y + 56};
        s.click = [] {};
        s.paint = [this, left](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool hot, bool) {
            float t0 = r.left + 10, t1 = r.right - 10, cy = r.top + 18;
            float t = std::clamp((pendingScale - 0.8f) / 0.6f, 0.0f, 1.0f);
            float x = t0 + (t1 - t0) * t;
            draw::RoundRect(dc, {t0, cy - 2.5f, t1, cy + 2.5f}, 2.5f, Rgba(255, 255, 255, 0.14f));
            draw::RoundRect(dc, {t0, cy - 2.5f, x, cy + 2.5f}, 2.5f, kAccent);
            for (float mark : {0.9f, 1.0f, 1.15f}) {
                float mx = t0 + (t1 - t0) * ((mark - 0.8f) / 0.6f);
                dc->FillEllipse(D2D1::Ellipse({mx, cy + 14}, 1.5f, 1.5f), draw::Brush(dc, kTextFaint));
                const wchar_t* name = mark < 0.95f ? L"Küçük" : mark < 1.05f ? L"Normal" : L"Büyük";
                draw::Text(dc, name, {mx - 30, cy + 17, mx + 30, cy + 33}, 10, kTextFaint, DWRITE_FONT_WEIGHT_NORMAL, Align::Center);
            }
            float rr = hot ? 9.0f : 8.0f;
            dc->FillEllipse(D2D1::Ellipse({x, cy}, rr, rr), draw::Brush(dc, Rgba(245, 245, 247)));
            draw::Text(dc, std::to_wstring(static_cast<int>(std::lround(pendingScale * 100))) + L"%", {r.right - 70, r.top - 14, r.right - 6, r.top + 2},
                       11.5f, kText, DWRITE_FONT_WEIGHT_SEMI_BOLD, Align::Trailing, true);
        };
        s.slide = [this, left, right](PointF p) {
            float t0 = left + 10, t1 = right - 10;
            float t = std::clamp((p.x - t0) / (t1 - t0), 0.0f, 1.0f);
            float v = 0.8f + 0.6f * t;
            for (float mark : {0.9f, 1.0f, 1.15f})
                if (std::fabs(v - mark) < 0.015f) v = mark; // sticks to the presets
            pendingScale = v;
            self->Invalidate();
        };
        s.slideEnd = [this] {
            auto& cur = Store::Get().settings();
            if (std::fabs(cur.uiScale - pendingScale) < 0.001) return;
            cur.uiScale = pendingScale;
            Store::Get().SaveSettings();
            App::Get().ApplySettings(); // relayouts everything and closes this page ...
            ui::Post([] { App::Get().Launcher().OpenSettings(); }); // ... so it is opened again at the new size
        };
        hits.push_back(std::move(s));
        y += 62;
    }

    // ---- search engine: a drop-down list
    section(L"Arama motoru");
    {
        int idx = static_cast<int>(hits.size());
        std::wstring current = web::FindEngine(st.searchEngine).name;
        Hit c;
        c.r = {left, y, right, y + 38};
        c.selectable = true;
        c.hint = L"Arama çubuğundaki web aramasında kullanılacak arama motoru.";
        c.paint = [this, idx, current](ID2D1DeviceContext* dc, D2D1_RECT_F r, bool, bool sel) {
            float hv = anim.Get(gfx::HoverBank::Hover, idx), pr = anim.Get(gfx::HoverBank::Press, idx);
            draw::RoundRect(dc, r, 10, Rgba(255, 255, 255, 0.05f + 0.05f * hv + 0.04f * pr));
            dc->DrawRoundedRectangle({{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, 10, 10},
                                     draw::Brush(dc, Rgba(255, 255, 255, 0.06f + 0.06f * hv)), 1);
            draw::Glyph(dc, g::Search, {r.left + 20, (r.top + r.bottom) / 2}, 13, kTextDim);
            draw::Text(dc, current, {r.left + 40, r.top, r.right - 40, r.bottom}, 13, kText);
            draw::Glyph(dc, g::ChevronDown, {r.right - 20, (r.top + r.bottom) / 2 + 0.5f * hv}, 11, kTextDim);
            if (sel) draw::FocusRing(dc, r, 10);
        };
        c.click = [this, idx, redo] {
            const auto& hit = hits[idx];
            std::vector<MenuItem> menu;
            for (const auto& e : web::Engines()) {
                MenuItem m;
                m.text = e.name;
                m.checked = Store::Get().settings().searchEngine == e.id;
                std::string id = e.id;
                m.action = [id, redo] {
                    Store::Get().settings().searchEngine = id;
                    Store::Get().SaveSettings();
                    redo();
                };
                menu.push_back(std::move(m));
            }
            POINT at = self->DipToScreen({hit.r.left, hit.r.bottom - scroll.value + 4});
            MenuPopup::Get().Show(std::move(menu), at);
        };
        hits.push_back(std::move(c));
        y += 46;
    }

    // ---- other
    section(L"Diğer");
    rowTop = y;
    tile(g::Restore, L"Görev çubuğunu geri getir", L"Gizlemeyi kapatır ve Windows görev çubuğunu hemen gösterir.", -1, true, false, [this, redo] {
        App::Get().RestoreTaskbarNow();
        redo();
    });
    tile(g::Info, L"Nerede ne var?", L"Çentik, alt alan ve kısayolların kısa rehberi.", -1, true, false,
         [] { ui::Post([] { App::Get().ShowGuide(); }); });
    tile(g::Power, L"NotchBar'dan çık", L"Uygulamayı kapatır ve görev çubuğunu geri getirir.", -1, true, true,
         [] { ui::Post([] { App::Get().Quit(); }); });
    endTiles();
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
    // Web search is always the first entry: Up from the first result lands on it.
    section(L"WEB'DE ARA");
    {
        Hit w;
        w.r = {pr.left + 6, y, pr.right - 6, y + 38};
        w.selectable = true;
        w.noDefault = true; // reachable with the Up key from the first result; Enter still opens the first result
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
        BuildHeader(L"Ayarlar", true, [this] {
            if (self->settingsOnly && self->requestClose) self->requestClose(false);
            else self->SetView(View::Favorites);
        });
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
                if (hits[i].scrolls && !hits[i].noDefault) {
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
    if (d.sliderHit >= 0 && d.sliderHit < static_cast<int>(d.hits.size()) && GetKeyState(VK_LBUTTON) < 0) {
        if (d.hits[d.sliderHit].slide) d.hits[d.sliderHit].slide(p);
        return;
    }
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
        d.anim.SetHot(h < 0 ? gfx::HoverBank::kNone : h);
        RequestAnimationFrame();
        Invalidate();
    }
}

void LauncherPanel::OnMouseLeave() {
    d_->hot = -1;
    d_->anim.SetHot(gfx::HoverBank::kNone);
    d_->anim.SetPressed(gfx::HoverBank::kNone);
    RequestAnimationFrame();
    Invalidate();
    if (onPointerLeave) onPointerLeave();
}

void LauncherPanel::OnMouseDown(PointF p, int button) {
    auto& d = *d_;
    if (MenuPopup::Get().IsOpen()) MenuPopup::Get().Close(true);
    d.downHit = d.HitAt(p);
    d.downPt = p;
    d.dragging = false;
    d.anim.SetPressed(d.downHit < 0 ? gfx::HoverBank::kNone : d.downHit);
    RequestAnimationFrame();
    if (button == 0 && d.downHit >= 0 && d.hits[d.downHit].slide) {
        d.sliderHit = d.downHit;
        d.hits[d.downHit].slide(p);
    }
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
    d.anim.SetPressed(gfx::HoverBank::kNone);
    RequestAnimationFrame();
    if (d.sliderHit >= 0 && button == 0) {
        int sh = d.sliderHit;
        d.sliderHit = -1;
        d.downHit = -1;
        if (sh < static_cast<int>(d.hits.size())) {
            if (d.hits[sh].slide) d.hits[sh].slide(p);
            auto end = d.hits[sh].slideEnd;
            if (end) end();
        }
        return;
    }
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
