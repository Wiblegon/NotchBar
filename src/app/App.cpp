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
#include "sys/MediaMonitor.h"
#include "sys/NetworkMonitor.h"
#include "sys/NotificationMonitor.h"
#include "sys/PowerMonitor.h"
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
        case ui::NotchButton::Network: sys::NetworkMonitor::OpenSystemUi(); break;
        case ui::NotchButton::Clock: ShellExecuteW(nullptr, L"open", L"ms-settings:dateandtime", nullptr, nullptr, SW_SHOWNORMAL); break;
        default: break;
        }
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
    tracker.Tap([](DWORD ev, HWND h) {
        if (ev == EVENT_OBJECT_SHOW) sys::TaskbarController::Get().OnTaskbarWindowShown(h);
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
    popups_ = std::make_unique<ui::PopupHost>();
    popups_->Init();
    popups_->onOpenChanged = [this] { UpdatePins(); };
    toasts_ = std::make_unique<ui::ToastHost>();
    toasts_->Init();
    ui::MenuPopup::Get().onClosed = [this] { UpdatePins(); };
    Store::Get().Subscribe([this] {
        if (launcher_) launcher_->Refresh();
    });

    sys::NetworkMonitor::Get().Subscribe([this] { notch_->Changed(); });
    sys::AudioMonitor::Get().Subscribe([this] {
        notch_->Changed();
        if (popups_) popups_->Refresh(ui::PopupKind::Volume);
    });
    sys::PowerMonitor::Get().Subscribe([this] {
        notch_->Changed();
        if (popups_) popups_->Refresh(ui::PopupKind::Battery);
    });
    sys::NetworkMonitor::Get().Start();

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
}

void App::UpdatePins() {
    notch_->SetPinned(popups_ && popups_->AnyOpen());
    dock_->SetPinned(ui::MenuPopup::Get().IsOpen());
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

void App::OnWindowsNotifications(const std::vector<NotificationItem>& current, bool silent) {
    auto fresh = history_.SyncWindows(current, ui::NowMs());
    bool suppressed = silent || paused_ || mode_ == ScreenMode::Fullscreen || !Store::Get().settings().showToasts;
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
