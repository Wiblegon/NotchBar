#include <windows.h>

#include <cmath>

#include "core/AtomicFile.h"
#include "core/Json.h"
#include "core/Strings.h"
#include "model/Battery.h"
#include "model/Calc.h"
#include "model/Favorites.h"
#include "model/Notifications.h"
#include "model/PanelDirection.h"
#include "model/ScreenClassifier.h"
#include "model/Search.h"
#include "model/SettingsModel.h"
#include "model/VisibilityModel.h"
#include "model/WebSearch.h"
#include "test.h"

using namespace nb;

// ------------------------------------------------------------------ visibility state machine
TEST(edge_desktop_is_normal_and_ignores_hover) {
    EdgeBarModel m({150, 700});
    m.SetMode(ScreenMode::Desktop, 0);
    CHECK(m.Presence() == EdgePresence::Normal);
    m.PointerEnter(10);
    m.PointerLeave(20);
    CHECK(!m.Tick(5000));
    CHECK(m.Presence() == EdgePresence::Normal);
}

TEST(edge_maximized_collapses_then_peeks_on_hover_and_recollapses) {
    EdgeBarModel m({150, 700});
    m.SetMode(ScreenMode::Maximized, 1000);
    CHECK(m.Presence() == EdgePresence::Normal); // grace period first
    CHECK(m.NextDeadline().has_value());
    CHECK(m.Tick(1700));
    CHECK(m.Presence() == EdgePresence::Collapsed);
    m.PointerEnter(2000);
    CHECK(!m.Tick(2100)); // dwell not elapsed
    CHECK(m.Presence() == EdgePresence::Collapsed);
    CHECK(m.Tick(2150));
    CHECK(m.Presence() == EdgePresence::Normal);
    m.PointerLeave(3000);
    CHECK(!m.Tick(3500));
    CHECK(m.Presence() == EdgePresence::Normal); // short grace keeps it open
    m.PointerEnter(3600); // came back in time
    CHECK(!m.Tick(5000));
    CHECK(m.Presence() == EdgePresence::Normal);
    m.PointerLeave(6000);
    CHECK(m.Tick(6700));
    CHECK(m.Presence() == EdgePresence::Collapsed);
}

TEST(edge_brief_touch_does_not_expand) {
    EdgeBarModel m({150, 700});
    m.SetMode(ScreenMode::Maximized, 0);
    m.Tick(700);
    m.PointerEnter(1000);
    m.PointerLeave(1050);
    CHECK(!m.Tick(1200));
    CHECK(m.Presence() == EdgePresence::Collapsed);
}

TEST(edge_fullscreen_hides_and_ignores_pointer) {
    EdgeBarModel m({150, 700});
    m.SetMode(ScreenMode::Fullscreen, 0);
    CHECK(m.Presence() == EdgePresence::Hidden);
    m.PointerEnter(10);
    CHECK(!m.Tick(10000));
    CHECK(m.Presence() == EdgePresence::Hidden);
    CHECK(!m.Hovered());
}

TEST(edge_fullscreen_exit_restores_mode_state) {
    EdgeBarModel m({150, 700});
    m.SetMode(ScreenMode::Fullscreen, 0);
    m.SetMode(ScreenMode::Desktop, 100);
    CHECK(m.Presence() == EdgePresence::Normal);
    m.SetMode(ScreenMode::Fullscreen, 200);
    m.SetMode(ScreenMode::Maximized, 300);
    CHECK(m.Presence() == EdgePresence::Collapsed); // never peeks by itself after fullscreen
}

TEST(edge_pinned_popup_keeps_normal) {
    EdgeBarModel m({150, 700});
    m.SetMode(ScreenMode::Maximized, 0);
    m.Tick(700);
    m.PointerEnter(1000);
    m.Tick(1200);
    m.SetPinned(true, 1300);
    m.PointerLeave(1400);
    CHECK(!m.Tick(5000));
    CHECK(m.Presence() == EdgePresence::Normal);
    m.SetPinned(false, 6000);
    CHECK(m.Tick(6700));
    CHECK(m.Presence() == EdgePresence::Collapsed);
}

// ------------------------------------------------------------------ panel direction
TEST(panel_direction_from_movement) {
    RectF bar{100, 500, 520, 546};
    CHECK(DeterminePanelDirection({300, 502}, bar, PointF{300, 470}) == PanelDir::Below);
    CHECK(DeterminePanelDirection({300, 544}, bar, PointF{300, 580}) == PanelDir::Above);
    // Movement wins over entry edge (fast pointer skipped the top band)
    CHECK(DeterminePanelDirection({300, 530}, bar, PointF{290, 480}) == PanelDir::Below);
}

TEST(panel_direction_from_entry_edge_and_side_default) {
    RectF bar{100, 500, 520, 546};
    CHECK(DeterminePanelDirection({300, 501}, bar, std::nullopt) == PanelDir::Below);
    CHECK(DeterminePanelDirection({300, 545}, bar, std::nullopt) == PanelDir::Above);
    // side approach: horizontal motion, middle height
    CHECK(DeterminePanelDirection({101, 523}, bar, PointF{60, 522}) == PanelDir::Below);
    CHECK(DeterminePanelDirection({101, 523}, bar, PointF{60, 522}, PanelDir::Above) == PanelDir::Above);
}

TEST(panel_direction_fit_flips_when_no_room) {
    CHECK(FitPanelDirection(PanelDir::Below, 600, 100, 300) == PanelDir::Above);
    CHECK(FitPanelDirection(PanelDir::Above, 50, 600, 300) == PanelDir::Below);
    CHECK(FitPanelDirection(PanelDir::Below, 400, 400, 300) == PanelDir::Below);
}

TEST(panel_direction_locked_while_open) {
    HoverPanelModel m({180, 450});
    m.PointerEnterBar(PanelDir::Above, 0);
    CHECK(m.Tick(180));
    CHECK(m.IsOpen());
    CHECK(m.Dir() == PanelDir::Above);
    // moving through the bridge gap into the panel, then back across the bar from the other side
    m.PointerLeaveAll(200);
    m.PointerEnterPanel(250);
    CHECK(m.GetState() == HoverPanelModel::State::Open);
    m.PointerEnterBar(PanelDir::Below, 300);
    CHECK(m.Dir() == PanelDir::Above);
    m.PointerLeaveAll(400);
    m.PointerEnterBar(PanelDir::Below, 600); // re-entered during close grace
    CHECK(m.Dir() == PanelDir::Above);
    CHECK(m.IsOpen());
    m.PointerLeaveAll(700);
    CHECK(m.Tick(1150));
    CHECK(!m.IsOpen());
    m.PointerEnterBar(PanelDir::Below, 2000); // new interaction decides again
    m.Tick(2180);
    CHECK(m.Dir() == PanelDir::Below);
}

TEST(panel_open_delay_cancels_on_quick_pass) {
    HoverPanelModel m({180, 450});
    m.PointerEnterBar(PanelDir::Below, 0);
    m.PointerLeaveAll(100);
    CHECK(!m.Tick(1000));
    CHECK(!m.IsOpen());
}

// ------------------------------------------------------------------ calculator
static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

TEST(calc_basic_operations_and_precedence) {
    CHECK(Near(calc::Evaluate(L"2+3*4").value, 14));
    CHECK(Near(calc::Evaluate(L"(2+3)*4").value, 20));
    CHECK(Near(calc::Evaluate(L"10/4").value, 2.5));
    CHECK(Near(calc::Evaluate(L"7-10").value, -3));
    CHECK(Near(calc::Evaluate(L"-(3+2)*2").value, -10));
    CHECK(Near(calc::Evaluate(L"2 × 3 ÷ 4").value, 1.5));
    CHECK(Near(calc::Evaluate(L"((1+2)*(3+4))/7").value, 3));
}

TEST(calc_decimals_and_locale_separators) {
    CHECK(Near(calc::Evaluate(L"1,5+1,5").value, 3));
    CHECK(Near(calc::Evaluate(L"1.5+1.5").value, 3));
    CHECK(Near(calc::Evaluate(L"1.234,5*2").value, 2469));
    CHECK(Near(calc::Evaluate(L"1,234.5*2").value, 2469));
    CHECK(Near(calc::Evaluate(L"1.000.000/4").value, 250000));
    CHECK(calc::Format(2.5, L',') == L"2,5");
    CHECK(calc::Format(0.1 + 0.2, L',') == L"0,3");
    CHECK(calc::Format(-0.0, L',') == L"0");
    CHECK(calc::Format(1e20, L',') == L"1e+20");
}

TEST(calc_errors_are_explicit) {
    CHECK(calc::Evaluate(L"5/0").status == calc::Status::DivideByZero);
    CHECK(calc::Evaluate(L"5/(2-2)").status == calc::Status::DivideByZero);
    CHECK(calc::Evaluate(L"2+").status == calc::Status::Incomplete);
    CHECK(calc::Evaluate(L"(2+3").status == calc::Status::Incomplete);
    CHECK(calc::Evaluate(L"2+3)").status == calc::Status::Invalid);
    CHECK(calc::Evaluate(L"()+1").status == calc::Status::Invalid);
    CHECK(calc::Evaluate(L"1..2+1").status == calc::Status::Invalid);
    CHECK(calc::Evaluate(L"spotify").status == calc::Status::NotExpression);
    CHECK(calc::Evaluate(L"42").status == calc::Status::NotExpression);
    CHECK(calc::Evaluate(L"-5").status == calc::Status::NotExpression);
    CHECK(calc::Evaluate(L"rm -rf / ; 2+2").status == calc::Status::NotExpression); // no code paths at all
    CHECK(calc::Evaluate(L"1e308*1e308").status != calc::Status::Ok);
}

// ------------------------------------------------------------------ favorites + persistence
TEST(favorites_add_remove_reorder) {
    Favorites f;
    CHECK(f.AddApp({L"a", L"A"}));
    CHECK(f.AddApp({L"b", L"B"}));
    CHECK(f.AddApp({L"c", L"C"}));
    CHECK(!f.AddApp({L"a", L"A"})); // no duplicates
    CHECK(f.MoveRoot(2, 0));
    CHECK(f.Items()[0].app.id == L"c");
    CHECK(f.RemoveApp(L"b"));
    CHECK(f.Items().size() == 2);
}

TEST(favorites_folders_lifecycle) {
    Favorites f;
    f.AddApp({L"a", L"A"});
    f.AddApp({L"b", L"B"});
    f.AddApp({L"c", L"C"});
    auto id = f.CreateFolder(L"  Oyunlar ", {L"b", L"c"});
    CHECK(f.Items().size() == 2);
    CHECK(f.Items()[1].kind == FavItem::Kind::Folder);
    CHECK(f.FindFolder(id)->folderName == L"Oyunlar");
    CHECK(f.RenameFolder(id, L"Oyun"));
    CHECK(!f.RenameFolder(id, L"   "));
    CHECK(f.MoveAppToFolder(L"a", id));
    CHECK(f.FindFolder(id)->children.size() == 3);
    CHECK(f.Locate(L"a")->first == id);
    CHECK(f.MoveAppToRoot(L"b"));
    CHECK(f.DeleteFolder(id)); // apps survive deletion
    CHECK(f.Contains(L"a") && f.Contains(L"b") && f.Contains(L"c"));
    CHECK(f.FindFolder(id) == nullptr);
}

static std::wstring TempPath(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"nb_test_" + name;
}

TEST(favorites_roundtrip_through_atomic_file) {
    Favorites f;
    f.AddApp({L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App", L"Hesap Makinesi"});
    f.AddApp({L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Spotify\\Spotify.exe", L"Spotify"});
    auto fid = f.CreateFolder(L"Müzik & Şarkılar", {L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Spotify\\Spotify.exe"});
    auto path = TempPath(L"fav.json");
    CHECK(file::WriteAtomic(path, json::Serialize(f.ToJson())));
    auto text = file::Read(path);
    CHECK(text.has_value());
    auto v = json::Parse(*text);
    CHECK(v.has_value());
    auto g = Favorites::FromJson(*v);
    CHECK(g.Items().size() == 2);
    CHECK(g.FindFolder(fid) && g.FindFolder(fid)->folderName == L"Müzik & Şarkılar");
    CHECK(g.Contains(L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"));
    // new folders after reload get fresh ids
    auto fid2 = g.CreateFolder(L"x");
    CHECK(fid2 != fid);
    DeleteFileW(path.c_str());
}

TEST(favorites_tolerates_bad_entries) {
    auto v = json::Parse(R"({"version":1,"items":[{"type":"app","id":"a","name":"A"},{"type":"app","id":"a"},
        {"type":"app"},{"type":"weird"},{"type":"folder","name":"F","items":[{"type":"app","id":"b"},
        {"type":"folder","items":[{"type":"app","id":"c"}]}]}]})");
    CHECK(v.has_value());
    std::vector<std::string> warn;
    auto f = Favorites::FromJson(*v, &warn);
    CHECK(f.Items().size() == 2);
    CHECK(f.Contains(L"b") && f.Contains(L"c"));
    CHECK(!warn.empty());
}

TEST(settings_recover_from_corrupt_file) {
    std::string garbage = "{\"version\":2, \"hideTaskbar\": tru";
    auto r = ParseSettings(&garbage);
    CHECK(r.status == LoadStatus::Corrupt);
    CHECK(r.settings.hideTaskbar == Settings{}.hideTaskbar); // safe defaults
    auto missing = ParseSettings(nullptr);
    CHECK(missing.status == LoadStatus::Missing);
    std::string wrongTypes = R"({"version":2,"hideTaskbar":"yes","uiScale":99,"searchEngine":"bing","timings":{"panelOpenDelayMs":-5}})";
    auto w = ParseSettings(&wrongTypes);
    CHECK(w.status == LoadStatus::Ok);
    CHECK(w.settings.hideTaskbar == true);
    CHECK(w.settings.uiScale <= 1.4);
    CHECK(w.settings.searchEngine == "bing");
    CHECK(w.settings.panelOpenDelayMs == 0);
}

TEST(settings_migrate_v1_keeps_values) {
    std::string v1 = R"({"version":1,"hideTaskbar":false,"panelOpenDelayMs":300})";
    auto r = ParseSettings(&v1);
    CHECK(r.status == LoadStatus::Migrated);
    CHECK(r.settings.hideTaskbar == false);
    CHECK(r.settings.panelOpenDelayMs == 300);
    auto back = ParseSettings(new std::string(json::Serialize(r.settings.ToJson())));
    CHECK(back.status == LoadStatus::Ok && back.settings.panelOpenDelayMs == 300);
}

TEST(atomic_write_replaces_existing) {
    auto path = TempPath(L"atomic.txt");
    CHECK(file::WriteAtomic(path, "first"));
    CHECK(file::WriteAtomic(path, "second"));
    CHECK(*file::Read(path) == "second");
    CHECK(GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES);
    auto q = file::Quarantine(path);
    CHECK(GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);
    DeleteFileW(q.c_str());
}

// ------------------------------------------------------------------ battery
TEST(battery_unknown_is_not_zero) {
    CHECK(battery::FormatPercent(std::nullopt) != battery::FormatPercent(0));
    CHECK(battery::FormatPercent(0) == L"0%");
    uint8_t zero = 0, unknown = 0xFF, reserved = 101;
    CHECK(battery::ParseBasBatteryLevel(&zero, 1) == 0);
    CHECK(!battery::ParseBasBatteryLevel(&reserved, 1).has_value());
    CHECK(!battery::ParseBasBatteryLevel(nullptr, 0).has_value());
    // system: 255 = unknown percentage; 128 = no battery (desktop)
    auto s = battery::FromSystemPower(1, 255, 255);
    CHECK(s && !s->percent.has_value());
    CHECK(!battery::FromSystemPower(1, 128, 255).has_value());
    auto e = battery::FromSystemPower(0, 4, 0);
    CHECK(e && e->percent == 0 && e->charge == battery::Charge::Discharging);
    auto c = battery::FromSystemPower(1, 8, 55);
    CHECK(c && c->charge == battery::Charge::Charging && c->percent == 55);
    (void)unknown;
}

TEST(battery_hyperx_parsing) {
    using namespace battery::hyperx;
    auto q = BuildQuery(kCmdBattery);
    CHECK(q.size() == 64 && q[0] == 0x0C && q[1] == 0x02 && q[2] == 0x03 && q[3] == 0x01 && q[5] == 0x06);
    uint8_t r[64] = {0x0C, 0x02, 0x03, 0x01, 0x00, 0x06, 89};
    auto a = ParseReport(r, sizeof r);
    CHECK(a.percent == 89);
    r[6] = 0;
    CHECK(ParseReport(r, sizeof r).percent == 0); // real zero
    r[6] = 0xFF;
    auto none = ParseReport(r, sizeof r);
    CHECK(!none.percent.has_value()); // "no value" stays unknown
    uint8_t ch[64] = {0x0C, 0x02, 0x03, 0x01, 0x00, 0x48, 1};
    CHECK(ParseReport(ch, sizeof ch).charge == battery::Charge::Charging);
    ch[6] = 2;
    CHECK(ParseReport(ch, sizeof ch).charge == battery::Charge::Full);
    uint8_t n[64] = {0x0D, 0, 0, 0, 1, 42};
    CHECK(ParseReport(n, sizeof n).percent == 42);
    uint8_t conn[64] = {0x0D, 0, 0, 0, 12, 0};
    CHECK(ParseReport(conn, sizeof conn).headsetConnected == false);
    uint8_t other[64] = {0x05, 1, 2, 3, 4, 5, 6};
    auto o = ParseReport(other, sizeof other);
    CHECK(!o.percent && !o.charge && !o.headsetConnected);
    CHECK(!ParseReport(r, 3).percent.has_value());
}

// ------------------------------------------------------------------ search
TEST(search_stale_results_are_rejected) {
    search::QueryGate gate;
    auto t1 = gate.Begin(); // "spo"
    auto t2 = gate.Begin(); // "spot"
    CHECK(!gate.IsCurrent(t1));
    CHECK(t1.IsCancelled());
    CHECK(gate.IsCurrent(t2));
    gate.CancelAll(); // launcher closed
    CHECK(!gate.IsCurrent(t2));
}

TEST(search_fold_and_rank) {
    CHECK(search::Fold(L"Görev Yöneticisi") == L"gorev yoneticisi");
    CHECK(search::Fold(L"İnternet ışık") == L"internet isik");
    auto q = search::Fold(L"spo");
    CHECK(search::RankName(search::Fold(L"Spotify"), q) > search::RankName(search::Fold(L"Microsoft Sports"), q));
    CHECK(search::RankName(search::Fold(L"Visual Studio Code"), L"vsc") > 0);
    CHECK(search::RankName(search::Fold(L"Notepad"), L"xyz") == 0);
}

TEST(web_search_url_encoding) {
    auto u = web::BuildSearchUrl("google", L"çağrı & şimşek?=1");
    CHECK(u == L"https://www.google.com/search?q=%C3%A7a%C4%9Fr%C4%B1%20%26%20%C5%9Fim%C5%9Fek%3F%3D1");
    auto d = web::BuildSearchUrl("unknown-engine", L"a\"b|c");
    CHECK(d.rfind(L"https://", 0) == 0);
    CHECK(d.find(L'"') == std::wstring::npos && d.find(L'|') == std::wstring::npos);
}

// ------------------------------------------------------------------ screen classification
static WindowFacts Win(RectI r) {
    WindowFacts w;
    w.frame = r;
    w.hasCaption = true;
    w.hasThickFrame = true;
    return w;
}

TEST(screen_classifier_maximized_vs_fullscreen) {
    RectI mon{0, 0, 1920, 1080};
    auto normal = Win({100, 100, 900, 700});
    CHECK(ClassifyWindow(normal, mon) == WindowClass::Normal);
    auto maxed = Win({0, 0, 1920, 1080}); // auto-hidden taskbar: maximized covers the monitor
    maxed.zoomed = true;
    CHECK(ClassifyWindow(maxed, mon) == WindowClass::Maximized);
    auto maxedWork = Win({0, 0, 1920, 1032});
    maxedWork.zoomed = true;
    CHECK(ClassifyWindow(maxedWork, mon) == WindowClass::Maximized);
    WindowFacts borderless; // WS_POPUP game
    borderless.frame = mon;
    CHECK(ClassifyWindow(borderless, mon) == WindowClass::Fullscreen);
    auto f11 = Win({0, 0, 1920, 1080}); // browser F11: frame styles removed
    f11.hasCaption = false;
    f11.hasThickFrame = false;
    f11.zoomed = true;
    CHECK(ClassifyWindow(f11, mon) == WindowClass::Fullscreen);
    auto overlay = borderless;
    overlay.clickThrough = true;
    CHECK(ClassifyWindow(overlay, mon) == WindowClass::Ignore);
    auto cloaked = borderless;
    cloaked.cloaked = true;
    CHECK(ClassifyWindow(cloaked, mon) == WindowClass::Ignore);
    auto other = Win({1920, 0, 3840, 1080}); // other monitor (also negative coords elsewhere)
    CHECK(ClassifyWindow(other, mon) == WindowClass::Ignore);
    auto neg = Win({-1920, 0, 0, 1080});
    neg.zoomed = true;
    CHECK(ClassifyWindow(neg, RectI{-1920, 0, 0, 1080}) == WindowClass::Maximized);
}

TEST(screen_mode_from_zorder) {
    using W = WindowClass;
    CHECK(ModeFromZOrder({W::Ignore, W::Fullscreen, W::Normal}, false) == ScreenMode::Fullscreen);
    CHECK(ModeFromZOrder({W::Normal, W::Maximized}, false) == ScreenMode::Maximized);
    CHECK(ModeFromZOrder({W::Normal, W::Fullscreen}, false) == ScreenMode::Maximized);
    CHECK(ModeFromZOrder({W::Normal, W::Normal}, false) == ScreenMode::Desktop);
    CHECK(ModeFromZOrder({}, false) == ScreenMode::Desktop);
    CHECK(ModeFromZOrder({W::Normal}, true) == ScreenMode::Fullscreen);
}

// ------------------------------------------------------------------ notifications
TEST(notifications_sync_and_toast_policy) {
    NotificationHistory h;
    NotificationItem a;
    a.windowsId = 1;
    a.title = L"A";
    a.arrivedMs = 10;
    NotificationItem b = a;
    b.windowsId = 2;
    b.arrivedMs = 20;
    auto fresh = h.SyncWindows({a, b}, 100);
    CHECK(fresh.size() == 2);
    CHECK(h.SyncWindows({a, b}, 200).empty());
    CHECK(h.SyncWindows({b}, 300).empty()); // a dismissed in Windows
    CHECK(h.Items().size() == 1);
    h.RemoveLocal(h.Items().front().key);     // hidden locally, still exists in Windows
    CHECK(h.SyncWindows({b}, 400).empty());   // and does not come back as "new"

    ToastPolicy p(2, 2, 5000);
    NotificationItem n;
    for (int i = 0; i < 10; ++i) {
        n.key = static_cast<uint64_t>(i + 1);
        p.Push(n, false, 0);
    }
    CHECK(p.VisibleItems().size() == 2);
    CHECK(p.QueuedCount() <= 2); // a burst never floods
    p.Clear();
    p.Push(n, true, 0); // fullscreen: suppressed, never replayed
    CHECK(p.VisibleItems().empty() && p.QueuedCount() == 0);
    p.Push(n, false, 0);
    CHECK(p.Tick(5000));
    CHECK(p.VisibleItems().empty());
}

TEST(json_roundtrip_unicode_and_errors) {
    auto v = json::Parse(R"({"a":"ç😀","b":[1,2.5,-3e2],"c":null,"d":true})");
    CHECK(v.has_value());
    CHECK((*v)["a"].asString() == "\xC3\xA7\xF0\x9F\x98\x80");
    CHECK((*v)["b"].asArray().size() == 3);
    auto again = json::Parse(json::Serialize(*v));
    CHECK(again.has_value() && (*again)["b"].asArray()[2].asNumber() == -300);
    CHECK(!json::Parse("{\"a\":1,}").has_value());
    CHECK(!json::Parse("[1 2]").has_value());
    CHECK(!json::Parse(std::string(200, '[')).has_value()); // depth guard
}
