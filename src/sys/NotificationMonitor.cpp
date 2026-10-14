#include "pch.h"

#include "sys/NotificationMonitor.h"

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include "core/Dispatcher.h"
#include "core/Log.h"

namespace nb::sys {
namespace {
using namespace winrt;
using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;

NotificationAccess FromStatus(UserNotificationListenerAccessStatus s) {
    switch (s) {
    case UserNotificationListenerAccessStatus::Allowed: return NotificationAccess::Allowed;
    case UserNotificationListenerAccessStatus::Denied: return NotificationAccess::Denied;
    default: return NotificationAccess::Unknown;
    }
}
} // namespace

NotificationMonitor& NotificationMonitor::Get() {
    static NotificationMonitor n;
    return n;
}

void NotificationMonitor::Start(std::function<void(std::vector<NotificationItem>, bool)> sink) {
    sink_ = std::move(sink);
    try {
        access_ = FromStatus(UserNotificationListener::Current().GetAccessStatus()); // never prompts
    } catch (...) {
        access_ = NotificationAccess::Unavailable;
    }
    LOGI("notifications: access=%d", static_cast<int>(access_));
}

void NotificationMonitor::Stop() {
    ui::ClearTimeout(timer_);
    ui::ClearTimeout(safety_);
    enabled_ = false;
}

void NotificationMonitor::SetEnabled(bool on) {
    enabled_ = on;
    if (!on) {
        ui::ClearTimeout(timer_);
        ui::ClearTimeout(safety_);
        return;
    }
    silentNext_ = true; // what is already there is not "new"
    TriggerRead(200);
}

void NotificationMonitor::SetPaused(bool p) {
    if (p == paused_) return;
    paused_ = p;
    if (p) {
        ui::ClearTimeout(timer_);
        ui::ClearTimeout(safety_);
    } else {
        // Coming back from fullscreen/lock: sync silently, do not replay everything as boxes.
        silentNext_ = true;
        TriggerRead(500);
    }
}

void NotificationMonitor::RequestAccess(std::function<void(NotificationAccess)> done) {
    // Only called when the user enables the feature. Must run on the UI (STA) thread.
    [](NotificationMonitor* self, std::function<void(NotificationAccess)> done) -> fire_and_forget {
        NotificationAccess a = NotificationAccess::Unavailable;
        try {
            auto st = co_await UserNotificationListener::Current().RequestAccessAsync();
            a = FromStatus(st);
        } catch (...) {
        }
        ui::Post([self, a, done] {
            self->access_ = a;
            LOGI("notifications: access after request=%d", static_cast<int>(a));
            if (done) done(a);
            if (a == NotificationAccess::Allowed) self->TriggerRead(100);
        });
    }(this, std::move(done));
}

void NotificationMonitor::TriggerRead(int delayMs) {
    if (!enabled_ || paused_ || access_ != NotificationAccess::Allowed) return;
    if (timer_) return;
    timer_ = ui::SetTimeout(delayMs, [this] {
        timer_ = 0;
        Read();
    });
}

void NotificationMonitor::ArmSafety() {
    ui::ClearTimeout(safety_);
    if (!enabled_ || paused_) return;
    safety_ = ui::SetTimeout(30000, [this] {
        safety_ = 0;
        TriggerRead(0);
    });
}

void NotificationMonitor::Read() {
    if (reading_) return;
    reading_ = true;
    bool silent = silentNext_;
    silentNext_ = false;
    [](NotificationMonitor* self, bool silent) -> fire_and_forget {
        std::vector<NotificationItem> out;
        bool ok = true;
        try {
            co_await resume_background();
            auto list = co_await UserNotificationListener::Current().GetNotificationsAsync(NotificationKinds::Toast);
            for (auto const& n : list) {
                NotificationItem it;
                it.source = NotificationSource::Windows;
                it.windowsId = n.Id();
                try {
                    auto info = n.AppInfo();
                    if (info) {
                        it.appName = info.DisplayInfo().DisplayName().c_str();
                        it.appId = info.AppUserModelId().c_str();
                    }
                } catch (...) {
                }
                auto binding = n.Notification().Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                if (binding) {
                    auto texts = binding.GetTextElements();
                    uint32_t i = 0;
                    for (auto const& t : texts) {
                        if (i == 0) it.title = t.Text().c_str();
                        else {
                            if (!it.body.empty()) it.body += L" ";
                            it.body += t.Text().c_str();
                        }
                        if (++i >= 4) break;
                    }
                }
                auto ft = winrt::clock::to_file_time(n.CreationTime());
                int64_t ms = static_cast<int64_t>(ft.value / 10000);
                it.arrivedMs = ms; // wall clock ms (only used for ordering and "time ago")
                out.push_back(std::move(it));
            }
        } catch (...) {
            ok = false;
        }
        ui::Post([self, out = std::move(out), ok, silent]() mutable {
            self->reading_ = false;
            if (ok && self->sink_ && self->enabled_) self->sink_(std::move(out), silent);
            self->ArmSafety();
        });
    }(this, silent);
}

} // namespace nb::sys
