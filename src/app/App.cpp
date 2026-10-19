#include "pch.h"

#include "app/App.h"

#include <wtsapi32.h>

#include "app/Messages.h"
#include "app/Startup.h"
#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Store.h"
#include "core/Worker.h"
#include "gfx/Anim.h"
#include "gfx/Draw.h"
#include "gfx/Graphics.h"
#include "gfx/IconCache.h"
#include "sys/AppCatalog.h"
#include "sys/AudioMonitor.h"
#include "sys/Autostart.h"
#include "sys/KeyboardHook.h"
#include "sys/MouseWatch.h"
#include "sys/MediaMonitor.h"
#include "sys/NetworkMonitor.h"
#include "sys/NotificationMonitor.h"
#include "sys/PowerMonitor.h"
#include "sys/PrivacyMonitor.h"
#include "sys/QuickTimer.h"
#include "sys/ScreenState.h"
#include "sys/TaskbarController.h"
#include "sys/Watchdog.h"
#include "sys/WindowTracker.h"
#include "sys/battery/BatteryService.h"
#include "ui/DockWindow.h"
#include "ui/Launcher.h"
#include "ui/NotchWindow.h"
#include "ui/Popups.h"
#include "ui/Toasts.h"
#include "ui/Widgets.h"

namespace nb {

App& App::Get() {
    static App a;
    return a;
}

bool App::CreateMessageWindow() {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = opts_.preview ? kMainClassPreview : kMainClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so broadcasts like TaskbarCreated,
    // WM_SETTINGCHANGE and WM_DISPLAYCHANGE reach us.
    msgWnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"NotchBar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                              wc.hInstance, nullptr);
    if (!msgWnd_) return false;
    ui::InitDispatcher(msgWnd_);
    // Let an elevated-integrity-free broadcast through UIPI for TaskbarCreated.
    ChangeWindowMessageFilterEx(msgWnd_, sys::TaskbarController::TaskbarCreatedMessage(), MSGFLT_ALLOW, nullptr);
    WTSRegisterSessionNotification(msgWnd_, NOTIFY_FOR_THIS_SESSION);
    return true;
}

static bool SystemAnimationsOff() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return !on;
}

int App::Run(const Options& o) {
    opts_ = o;
    startup::Begin();
    if (!CreateMessageWindow()) return 1;
    startup::Mark("window");

    Store::Get().Load();
    auto& st = Store::Get().settings();
    gfx::SetReducedMotion(st.reduceMotion || SystemAnimationsOff());
    startup::Mark("settings");

    if (!gfx::Graphics::Get().Init()) {
        MessageBoxW(nullptr, L"Grafik aygıtı başlatılamadı.", L"NotchBar", MB_ICONERROR);
        return 2;
    }
    draw::Init();
    ui::Screen::Current().Update(static_cast<float>(st.uiScale));
    gfx::Surface::SetGlobalUiScale(static_cast<float>(st.uiScale));
    gfx::IconCache::Get().SetOnLoaded([] {
        for (auto* s : gfx::Surface::All()) s->Invalidate();
    });
    startup::Mark("graphics");

    // Cheap, event-driven sources the notch needs for its first frame.
    sys::PowerMonitor::Get().Start(msgWnd_);
    sys::AudioMonitor::Get().Start();

    notch_ = std::make_unique<ui::NotchWindow>();
    dock_ = std::make_unique<ui::DockWindow>();
    notch_->Init();
    dock_->Init();
    ApplySettings();
    notch_->onClick = [this](ui::NotchButton b) {
        if (!popups_) return; // deferred services not up yet (first few ms)
        switch (b) {
        case ui::NotchButton::Media: popups_->Toggle(ui::PopupKind::Media, notch_->AnchorOf(b)); break;
        case ui::NotchButton::Volume: popups_->Toggle(ui::PopupKind::Volume, notch_->AnchorOf(b)); break;
        case ui::NotchButton::Battery: popups_->Toggle(ui::PopupKind::Battery, notch_->AnchorOf(b)); break;
        case ui::NotchButton::Notifications:
            popups_->Toggle(ui::PopupKind::Notifications, notch_->AnchorOf(b));
            history_.MarkAllSeen();
            notch_->Changed();
            break;
        case ui::NotchButton::Power: {
            auto run = [](const wchar_t* exe, const wchar_t* args) {
                return [=] { ShellExecuteW(nullptr, L"open", exe, args, nullptr, SW_HIDE); };
            };
            std::vector<ui::MenuItem> menu;
            menu.push_back({L"NotchBar ayarları", draw::glyph::Settings, [this] { ui::Post([this] { launcher_->OpenSettings(); }); }});
            menu.push_back({L"Kilitle", 0, [] { LockWorkStation(); }});
            menu.back().separatorBefore = true;
            menu.push_back({L"Uyku", 0, run(L"rundll32.exe", L"powrprof.dll,SetSuspendState 0,1,0")});
            menu.push_back({L"Oturumu kapat", 0, run(L"shutdown.exe", L"/l")});
            menu.back().separatorBefore = true;
            menu.push_back({L"Yeniden başlat", 0, run(L"shutdown.exe", L"/r /t 0")});
            menu.push_back({L"Kapat", draw::glyph::Power, run(L"shutdown.exe", L"/s /t 0")});
            POINT at = notch_->AnchorOf(b);
            at.x -= ui::Screen::Current().Px(130);
            ui::MenuPopup::Get().Show(std::move(menu), at);
            break;
        }
        case ui::NotchButton::Network: popups_->Toggle(ui::PopupKind::Network, notch_->AnchorOf(b)); break;
        case ui::NotchButton::Mic: ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-microphone", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ui::NotchButton::Camera: ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ui::NotchButton::Clock: popups_->Toggle(ui::PopupKind::Calendar, notch_->AnchorOf(b)); break;
        case ui::NotchButton::Tray: popups_->Toggle(ui::PopupKind::Tray, notch_->AnchorOf(b)); break;
        case ui::NotchButton::Quick: popups_->Toggle(ui::PopupKind::Quick, notch_->AnchorOf(b)); break;
        default: break;
        }
    };
    notch_->onContext = [this](POINT at) {
        if (popups_) ShowAppMenu(at);
    };
    gfx::Surface::RenderAll(ui::NowMs());
    startup::Mark("ui-visible");

    if (!opts_.preview && st.hideTaskbar) {
        sys::Watchdog::Get().Start(); // must exist before anything is changed
        sys::TaskbarController::Get().Hide();
    } else if (!opts_.preview) {
        // Setting is off but a previous run may have left the taskbar hidden.
        auto s = sys::TaskbarController::ReadState();
        if (s.hidden) sys::TaskbarController::RestoreFromState("startup: hiding disabled");
    }
    startup::Mark("taskbar");

    sys::KeyboardHook::Get().SetEnabled(st.captureWinKey);
    sys::KeyboardHook::Get().Start(msgWnd_, WM_NB_WINKEY);
    startup::Mark("hotkey");

    auto& tracker = sys::WindowTracker::Get();
    tracker.Subscribe([this] { dock_->Sync(); });
    tracker.Tap([this](DWORD ev, HWND h) {
        if (ev == EVENT_OBJECT_SHOW) sys::TaskbarController::Get().OnTaskbarWindowShown(h);
        // Other always-on-top windows (Start, task manager, overlays) can end up above the notch and
        // the app area; put them back on top shortly after any window change.
        if (ev == EVENT_SYSTEM_FOREGROUND || ev == EVENT_SYSTEM_MINIMIZEEND || ev == EVENT_SYSTEM_MOVESIZEEND ||
            ev == EVENT_OBJECT_SHOW)
            ScheduleRaise();
    });
    tracker.Start();
    sys::ScreenState::Get().OnChanged([this](ScreenMode m) { OnScreenMode(m); });
    sys::ScreenState::Get().SetMonitor(ui::Screen::Current().handle);
    sys::ScreenState::Get().Start();
    gfx::Surface::RenderAll(ui::NowMs());
    startup::Mark("windows");
    startup::Report(opts_.fromAutostart);

    ui::Post([this] { StartDeferred(); });
    Loop();
    return 0;
}

void App::StartDeferred() {
    // Things that are not needed for the first frame, in a controlled order.
    auto& catalog = sys::AppCatalog::Get();
    catalog.LoadCache();
    catalog.Subscribe([this] {
        Store::Get().favorites().UpdateNames(sys::AppCatalog::Get().Refs());
        if (launcher_) launcher_->Refresh();
    });
    launcher_ = std::make_unique<ui::Launcher>();
    launcher_->Init();
    launcher_->onOpenChanged = [this] { UpdatePins(); };
    popups_ =std::make_unique<ui::PopupHost>();
    popups_->Init();
    popups_->onOpenChanged = [this] { UpdatePins(); };
    toasts_ = std::make_unique<ui::ToastHost>();
    toasts_->Init();
    ui::MenuPopup::Get().onClosed = [this] { UpdatePins(); };
    ui::MenuPopup::Get().onOpened = [this] { UpdatePins(); };
    notch_->onBlankClick = [this] { CloseTransients(); };
    dock_->onBlankClick = [this] { CloseTransients(); };
    Store::Get().Subscribe([this] {
        if (launcher_) launcher_->Refresh();
    });

    sys::QuickTimer::Get().onChanged = [this] {
        if (popups_) popups_->Refresh(ui::PopupKind::Calendar);
    };
    sys::QuickTimer::Get().onFinished = [this](sys::TimerKind k) {
        PostInternalNotification(k == sys::TimerKind::Focus ? L"Odak oturumu bitti" : L"Zamanlayıcı bitti", L"");
        MessageBeep(MB_ICONASTERISK);
    };
    sys::NetworkMonitor::Get().Subscribe([this] {
        notch_->Changed();
        if (popups_) popups_->Refresh(ui::PopupKind::Network);
    });
    sys::AudioMonitor::Get().Subscribe([this] {
        notch_->Changed();
        if (popups_) popups_->Refresh(ui::PopupKind::Volume);
    });
    sys::PowerMonitor::Get().Subscribe([this] {
        notch_->Changed();
        if (popups_) popups_->Refresh(ui::PopupKind::Battery);
    });
    sys::NetworkMonitor::Get().Start();
    sys::PrivacyMonitor::Get().Subscribe([this] { notch_->Changed(); });
    sys::PrivacyMonitor::Get().SetPaused(paused_ || mode_ == ScreenMode::Fullscreen);
    sys::PrivacyMonitor::Get().Start();

    auto& media = sys::MediaMonitor::Get();
    media.Subscribe([this] {
        notch_->Changed();
        if (popups_) popups_->Refresh(ui::PopupKind::Media);
    });
    media.Start();

    sys::NotificationMonitor::Get().Start([this](std::vector<NotificationItem> items, bool silent) {
        OnWindowsNotifications(items, silent);
    });
    sys::WindowTracker::Get().Tap([](DWORD ev, HWND h) {
        // A toast window appearing is our cue to re-read notifications (no polling needed).
        if (ev != EVENT_OBJECT_SHOW) return;
        wchar_t cls[64];
        if (GetClassNameW(h, cls, 64) && wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0)
            sys::NotificationMonitor::Get().TriggerRead(600);
    });
    sys::NotificationMonitor::Get().SetEnabled(Store::Get().settings().windowsNotifications);

    // Devices last: they may touch Bluetooth/HID stacks; never block the UI.
    ui::SetTimeout(1500, [this] {
        if (!Store::Get().settings().deviceBattery) return;
        auto& b = sys::BatteryService::Get();
        b.Subscribe([this] {
            if (popups_) popups_->Refresh(ui::PopupKind::Battery);
        });
        b.Start();
        b.SetPaused(paused_ || mode_ == ScreenMode::Fullscreen);
    });
    ui::SetTimeout(3000, [] { sys::AppCatalog::Get().Refresh(); });
    startup::Mark("deferred");
    LOGI("deferred services started");

    if (!opts_.preview && !Store::Get().settings().firstRunDone) ui::SetTimeout(600, [this] { FirstRun(); });
}

void App::FirstRun() {
    auto& st = Store::Get().settings();
    TASKDIALOGCONFIG tc{sizeof tc};
    tc.hwndParent = nullptr;
    tc.dwFlags = TDF_VERIFICATION_FLAG_CHECKED | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"NotchBar";
    tc.pszMainInstruction = L"NotchBar hazır";
    tc.pszContent =
        L"Üstteki çentik saat, ses, bağlantı, pil, medya ve bildirimleri; alttaki alan açık uygulamaları gösterir. "
        L"Windows tuşu arama çubuğunu açar.\n\n"
        L"Windows görev çubuğu NotchBar açıkken gizlenir. Geri getirmek için: Windows tuşu → arama çubuğunun üzerine gelin → "
        L"Ayarlar → \"Windows görev çubuğunu geri getir\", ya da Win+R ile:  NotchBar.exe --restore-taskbar";
    tc.pszVerificationText = L"Windows'ta oturum açıldığında otomatik başlat (önerilir)";
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    BOOL autostart = TRUE;
    int button = 0;
    TaskDialogIndirect(&tc, &button, nullptr, &autostart);
    std::wstring err;
    SetAutostart(autostart != FALSE, &err);
    st.firstRunDone = true;
    Store::Get().SaveSettings();
}

void App::Loop() {
    MSG msg;
    for (;;) {
        bool animating = gfx::Surface::AnyAnimating();
        if (!animating) {
            // Idle: block until something happens. No timers, no frames.
            MsgWaitForMultipleObjectsEx(0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (gfx::Surface::AnyAnimating()) {
            gfx::Surface::RenderAll(ui::NowMs());
            DwmFlush(); // pace animation frames to the compositor (only while animating)
        }
    }
}

void App::Relayout() {
    auto& st = Store::Get().settings();
    ui::Screen::Current().Update(static_cast<float>(st.uiScale));
    gfx::Surface::SetGlobalUiScale(static_cast<float>(st.uiScale));
    sys::ScreenState::Get().SetMonitor(ui::Screen::Current().handle);
    notch_->Layout();
    dock_->Layout();
    if (launcher_) launcher_->Layout();
    if (toasts_) toasts_->Layout();
    if (popups_) popups_->CloseAll();
    LOGI("relayout: monitor %ldx%ld scale %.2f", ui::Screen::Current().monitor.right - ui::Screen::Current().monitor.left,
         ui::Screen::Current().monitor.bottom - ui::Screen::Current().monitor.top, ui::Screen::Current().scale);
}

void App::OnScreenMode(ScreenMode m) {
    mode_ = m;
    notch_->SetMode(m);
    dock_->SetMode(m);
    bool fs = m == ScreenMode::Fullscreen;
    if (fs) {
        if (popups_) popups_->CloseAll();
        if (toasts_) toasts_->Clear();
        ui::MenuPopup::Get().Close(false);
        ui::Tooltip::Get().Hide();
    }
    sys::NotificationMonitor::Get().SetPaused(paused_ || fs);
    sys::BatteryService::Get().SetPaused(paused_ || fs);
    sys::PrivacyMonitor::Get().SetPaused(paused_ || fs);
    // Behind a game: lowest scheduling priority and efficiency-core hint so NotchBar never competes
    // with it for CPU. Back to normal as soon as the game is not fullscreen.
    SetPriorityClass(GetCurrentProcess(), fs ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE pt{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                      static_cast<ULONG>(PROCESS_POWER_THROTTLING_EXECUTION_SPEED),
                                      static_cast<ULONG>(fs ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0)};
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

void App::SetPaused(bool p, const char* why) {
    if (p == paused_) return;
    paused_ = p;
    LOGI("%s (%s)", p ? "paused" : "resumed", why);
    if (p) {
        if (popups_) popups_->CloseAll();
        if (launcher_) launcher_->Close(false);
        if (toasts_) toasts_->Clear();
    } else {
        notch_->RefreshClock();
        sys::NetworkMonitor::Get().ScheduleEvaluate();
        sys::PowerMonitor::Get().Refresh();
        sys::ScreenState::Get().Schedule(100);
    }
    bool fs = mode_ == ScreenMode::Fullscreen;
    sys::NotificationMonitor::Get().SetPaused(p || fs);
    sys::BatteryService::Get().SetPaused(p || fs);
    sys::PrivacyMonitor::Get().SetPaused(p || fs);
}

void App::UpdatePins() {
    if (!notch_ || !dock_) return; // torn down (late callback from a static menu while quitting)
    // While the search bar is open (Windows key) the notch and the app area show in full, also in a
    // maximized window, so everything is reachable from one place.
    // The same goes for any open menu or box: the bars stay put while something of theirs is open.
    bool launcherOpen = launcher_ && launcher_->IsOpen();
    bool any = launcherOpen || (popups_ && popups_->AnyOpen()) || ui::MenuPopup::Get().IsOpen();
    notch_->SetPinned(any);
    dock_->SetPinned(any);
    // Clicks elsewhere on screen close whatever is open (watched only while something is open).
    auto& mw = sys::MouseWatch::Get();
    mw.onPress = [this](POINT pt) { OnPointerDown(pt); };
    mw.SetActive(any && !quitting_);
}

void App::CloseTransients() {
    if (popups_) popups_->CloseAll();
    ui::MenuPopup::Get().Close(false);
}

void App::OnPointerDown(POINT pt) {
    HWND h = WindowFromPoint(pt);
    HWND root = h ? GetAncestor(h, GA_ROOT) : nullptr;
    DWORD pid = 0;
    if (root) GetWindowThreadProcessId(root, &pid);
    if (pid == GetCurrentProcessId()) return; // our own windows handle their clicks themselves
    CloseTransients();
    if (launcher_ && launcher_->IsOpen()) launcher_->Close(false);
}

void App::ScheduleRaise() {
    if (raiseTimer_ || quitting_) return;
    raiseTimer_ = ui::SetTimeout(120, [this] {
        raiseTimer_ = 0;
        if (quitting_ || !notch_) return;
        notch_->Raise();
        dock_->Raise();
    });
}

void App::ShowAppMenu(POINT at) {
    std::vector<ui::MenuItem> menu;
    menu.push_back({L"Ayarlar", draw::glyph::Settings, [this] { ui::Post([this] { launcher_->OpenSettings(); }); }});
    menu.push_back({L"Nerede ne var?", 0, [this] { ui::Post([this] { ShowGuide(); }); }});
    menu.push_back({L"Görev çubuğunu geri getir", draw::glyph::Restore, [this] { ui::Post([this] { RestoreTaskbarNow(); }); }});
    menu.back().separatorBefore = true;
    menu.push_back({L"NotchBar'dan çık", draw::glyph::Power, [this] { ui::Post([this] { Quit(); }); }});
    ui::MenuPopup::Get().Show(std::move(menu), at);
}

void App::ShowGuide() {
    TASKDIALOGCONFIG tc{sizeof tc};
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"NotchBar";
    tc.pszMainInstruction = L"Nerede ne var?";
    tc.pszContent =
        L"ÜST ÇENTİK\n"
        L"Sol: medya (çalarken), ses, ağ, hızlı ayarlar (Bluetooth, Wi‑Fi, uçak modu, rahatsız etme...), arka plan uygulamaları.  Orta: saat (takvim, zamanlayıcı, odak).  "
        L"Sağ: pil / cihaz pilleri, bildirimler, güç menüsü (ayarlar da burada).\n"
        L"Çentiğe sağ tıklayınca ayarlar, görev çubuğunu geri getirme ve çıkış menüsü açılır.\n\n"
        L"ALT ALAN\nYalnızca açık uygulamalar. Simgeye tıkla: öne getir / küçült. Orta tık: kapat. Sağ tık: menü.\n\n"
        L"WINDOWS TUŞU\nArama çubuğunu açar; çentik ve alt alan da (büyütülmüş pencerede bile) görünür. "
        L"Çubuğun üzerine gelince favoriler paneli açılır, dişli simgesi ayarlara götürür.\n\n"
        L"BÜYÜTÜLMÜŞ PENCERE\nÜstte yalnızca küçük bir saat görünür; üzerine gelince çentik açılır. "
        L"Ayarlar'dan kapatılabilir (o zaman ince bir çizgi kalır).\n\n"
        L"TAM EKRAN (oyun / video)\nHer şey gizlenir ve hiçbir kaynak harcanmaz.";
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&tc, nullptr, nullptr, nullptr);
}

void App::PostInternalNotification(const std::wstring& title, const std::wstring& body) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    TimeMs wall = static_cast<TimeMs>((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10000);
    const auto& n = history_.AddInternal(title, body, wall);
    bool suppressed = paused_ || mode_ == ScreenMode::Fullscreen || !Store::Get().settings().showToasts;
    if (toasts_) toasts_->Push(n, suppressed);
    notch_->Changed();
    if (popups_) popups_->Refresh(ui::PopupKind::Notifications);
}

void App::ActivateNotification(const NotificationItem& n) {
    NotificationItem copy = n; // the history entry is removed below
    if (copy.source == NotificationSource::Windows && !copy.appId.empty()) sys::AppCatalog::Launch(copy.appId);
    DismissNotification(copy);
}

void App::DismissNotification(const NotificationItem& n) {
    if (n.source == NotificationSource::Windows) {
        sys::NotificationMonitor::Get().Remove(n.windowsId);
        sys::NotificationMonitor::Get().TriggerRead(500);
    }
    history_.RemoveLocal(n.key);
    notch_->Changed();
    if (popups_) popups_->Refresh(ui::PopupKind::Notifications);
}

void App::ClearNotifications() {
    sys::NotificationMonitor::Get().RemoveAll();
    std::vector<uint64_t> keys;
    for (const auto& n : history_.Items()) keys.push_back(n.key);
    for (auto k : keys) history_.RemoveLocal(k);
    sys::NotificationMonitor::Get().TriggerRead(500);
    notch_->Changed();
    if (popups_) popups_->Refresh(ui::PopupKind::Notifications);
}

void App::OnWindowsNotifications(const std::vector<NotificationItem>& current, bool silent) {
    auto fresh = history_.SyncWindows(current, ui::NowMs());
    bool suppressed = silent || paused_ || mode_ == ScreenMode::Fullscreen || !Store::Get().settings().showToasts ||
                      sys::QuickTimer::Get().Kind() == sys::TimerKind::Focus; // focus session: no toasts
    if (silent) history_.MarkAllSeen(); // existing notifications are not "new" after startup/fullscreen
    if (!fresh.empty()) LOGI("notifications: %zu new (%s)", fresh.size(), suppressed ? "listed only" : "shown");
    // Only the newest few become boxes; the rest are just listed in the notification box.
    size_t start = fresh.size() > 2 ? fresh.size() - 2 : 0;
    for (size_t i = start; i < fresh.size(); ++i)
        if (toasts_) toasts_->Push(fresh[i], suppressed);
    notch_->Changed();
    if (popups_) popups_->Refresh(ui::PopupKind::Notifications);
}

void App::ApplySettings() {
    auto& st = Store::Get().settings();
    gfx::SetReducedMotion(st.reduceMotion || SystemAnimationsOff());
    EdgeTimings et{st.edgeExpandDelayMs, st.edgeCollapseDelayMs};
    notch_->SetTimings(et);
    dock_->SetTimings(et);
    notch_->SetClockPill(st.maximizedClockOnly);
    sys::KeyboardHook::Get().SetEnabled(st.captureWinKey);
    static double applied = st.uiScale;
    if (std::fabs(applied - st.uiScale) > 0.001) {
        applied = st.uiScale;
        Relayout();
    }
}

void App::SetTaskbarHidden(bool hide) {
    auto& st = Store::Get().settings();
    st.hideTaskbar = hide;
    Store::Get().SaveSettings();
    if (opts_.preview) return; // preview never touches the real taskbar
    if (hide) {
        sys::Watchdog::Get().Start();
        sys::TaskbarController::Get().Hide();
    } else {
        sys::TaskbarController::Get().Restore();
        sys::Watchdog::Get().Stop();
    }
}

void App::RestoreTaskbarNow() {
    SetTaskbarHidden(false);
    PostInternalNotification(L"Windows görev çubuğu geri getirildi", L"Gizlemek için Ayarlar'dan yeniden açabilirsiniz.");
}

void App::PeekWindowsTray() {
    if (opts_.preview) return;
    if (!sys::TaskbarController::Get().IsHidden()) {
        // Already visible: just make sure the user sees it.
        for (HWND h : sys::TaskbarAdapter::FindTaskbars()) ShowWindow(h, SW_SHOWNA);
        return;
    }
    sys::TaskbarController::Get().PeekTray();
}

bool App::SetAutostart(bool on, std::wstring* error) {
    auto& st = Store::Get().settings();
    st.autostart = on;
    Store::Get().SaveSettings();
    if (opts_.preview) {
        if (error) *error = L"Önizleme modunda başlangıç kaydı oluşturulmaz.";
        return false;
    }
    return on ? sys::autostart::Enable(paths::ExePath(), error) : sys::autostart::Disable();
}

void App::EnableWindowsNotifications(bool on) {
    auto& st = Store::Get().settings();
    st.windowsNotifications = on;
    Store::Get().SaveSettings();
    auto& nm = sys::NotificationMonitor::Get();
    if (on && !st.notificationPermissionAsked) {
        st.notificationPermissionAsked = true;
        nm.RequestAccess([this](sys::NotificationAccess a) {
            if (a != sys::NotificationAccess::Allowed)
                PostInternalNotification(L"Bildirim erişimi verilmedi",
                                         L"Ayarlar > Gizlilik ve güvenlik > Bildirimler bölümünden izin verebilirsiniz.");
            if (launcher_) launcher_->Refresh();
        });
    }
    nm.SetEnabled(on);
}

void App::Quit() {
    if (quitting_) return;
    quitting_ = true;
    sys::MouseWatch::Get().SetActive(false);
    LOGI("quitting");
    if (!opts_.preview && sys::TaskbarController::Get().IsHidden()) sys::TaskbarController::Get().Restore();
    sys::Watchdog::Get().Stop();
    sys::KeyboardHook::Get().Stop();
    Store::Get().FlushNow();
    sys::BatteryService::Get().Stop();
    sys::NotificationMonitor::Get().Stop();
    sys::MediaMonitor::Get().Stop();
    sys::NetworkMonitor::Get().Stop();
    sys::AudioMonitor::Get().Stop();
    sys::PowerMonitor::Get().Stop();
    sys::WindowTracker::Get().Stop();
    WorkerPool::StopAll();
    launcher_.reset();
    popups_.reset();
    toasts_.reset();
    notch_.reset();
    dock_.reset();
    WTSUnRegisterSessionNotification(msgWnd_);
    PostQuitMessage(0);
}

LRESULT CALLBACK App::MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (App::Get().msgWnd_ == h || App::Get().msgWnd_ == nullptr) {
        if (App::Get().msgWnd_ == nullptr) return DefWindowProcW(h, m, wp, lp);
        return App::Get().OnMessage(m, wp, lp);
    }
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT App::OnMessage(UINT m, WPARAM wp, LPARAM lp) {
    if (m == sys::TaskbarController::TaskbarCreatedMessage() && m != 0) {
        sys::TaskbarController::Get().OnTaskbarCreated();
        PostInternalNotification(L"Explorer yeniden başladı", L"Görev çubuğu ayarı yeniden uygulandı.");
        return 0;
    }
    switch (m) {
    case ui::WM_NB_INVOKE: ui::HandleInvoke(lp); return 0;
    case ui::WM_NB_RENDER: gfx::Surface::RenderAll(ui::NowMs()); return 0;
    case WM_TIMER: ui::HandleTimer(wp); return 0;
    case WM_NB_WINKEY:
        if (launcher_ && !paused_) launcher_->Toggle();
        return 0;
    case WM_NB_SHOW_LAUNCHER:
        if (launcher_) launcher_->Open();
        return 0;
    case WM_NB_RESTORE_TASKBAR: RestoreTaskbarNow(); return 0;
    case WM_CLOSE: Quit(); return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        ui::ClearTimeout(relayoutTimer_);
        relayoutTimer_ = ui::SetTimeout(250, [this] {
            relayoutTimer_ = 0;
            Relayout();
        });
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) ui::Screen::Current().Update(static_cast<float>(Store::Get().settings().uiScale));
        if (wp == SPI_SETCLIENTAREAANIMATION) ApplySettings();
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"intl") == 0) notch_->RefreshClock();
        return 0;
    case WM_TIMECHANGE: notch_->RefreshClock(); return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE || wp == PBT_APMPOWERSTATUSCHANGE) sys::PowerMonitor::Get().Refresh();
        else if (wp == PBT_APMSUSPEND) SetPaused(true, "suspend");
        else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) SetPaused(false, "resume");
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        switch (wp) {
        case WTS_SESSION_LOCK:
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT: SetPaused(true, "session locked/disconnected"); break;
        case WTS_SESSION_UNLOCK:
        case WTS_CONSOLE_CONNECT:
        case WTS_REMOTE_CONNECT: SetPaused(false, "session unlocked/connected"); break;
        }
        return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            // Logoff/shutdown: keep the user's taskbar setting as is for the next logon; the
            // watchdog is told this is not a crash. Persist data now.
            sys::TaskbarController::Get().MarkSessionEnding();
            Store::Get().FlushNow();
            LOGI("session ending");
        }
        return 0;
    case WM_DEVICECHANGE: return TRUE;
    }
    return DefWindowProcW(msgWnd_, m, wp, lp);
}

} // namespace nb
