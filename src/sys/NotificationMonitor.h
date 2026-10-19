#pragma once
// Reads Windows notifications through UserNotificationListener.
//
// Findings on Windows 11 (unpackaged desktop app, verified with tools/probe):
//  * GetAccessStatus / GetNotificationsAsync work without package identity once the user allows
//    "notification access" for apps.
//  * The NotificationChanged event is NOT available without package identity
//    (subscribing fails with 0x80070490). Therefore changes are detected by
//      - an immediate re-read when the shell shows a toast window (WinEvent, no polling), and
//      - a slow safety re-read (every 30 s) while enabled and not paused.
// Permission is requested only when the user turns the feature on, never at startup.
#include <functional>
#include <vector>

#include "model/Notifications.h"

namespace nb::sys {

enum class NotificationAccess { Unknown, Allowed, Denied, Unavailable };

class NotificationMonitor {
public:
    static NotificationMonitor& Get();
    void Start(std::function<void(std::vector<NotificationItem>, bool silent)> sink);
    void Stop();
    void SetEnabled(bool on);
    void SetPaused(bool paused); // fullscreen / locked: no reads; the next read is "silent"
    void RequestAccess(std::function<void(NotificationAccess)> done);
    NotificationAccess Access() const { return access_; }
    void TriggerRead(int delayMs = 400);
    void Remove(uint32_t windowsId); // dismisses it in Windows too
    void RemoveAll();                // clears the Windows notification list

private:
    void Read();
    std::function<void(std::vector<NotificationItem>, bool)> sink_;
    NotificationAccess access_ = NotificationAccess::Unknown;
    bool enabled_ = false, paused_ = false, silentNext_ = true, reading_ = false;
    uint64_t timer_ = 0, safety_ = 0;
    void ArmSafety();
};

} // namespace nb::sys
