#pragma once
// Notification data model + toast presentation policy (pure).
// Windows notifications and this app's own notifications are separate sources.
// Hiding a toast box never removes the notification from Windows.
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "model/VisibilityModel.h"

namespace nb {

enum class NotificationSource { Windows, Internal };

struct NotificationItem {
    uint64_t key = 0;            // unique within this process
    NotificationSource source = NotificationSource::Internal;
    uint32_t windowsId = 0;      // UserNotification.Id (Windows source only)
    std::wstring appName;
    std::wstring appId;          // AUMID when known (icon lookup)
    std::wstring title;
    std::wstring body;
    TimeMs arrivedMs = 0;
};

// Bounded, in-memory history. Nothing is persisted or sent anywhere.
class NotificationHistory {
public:
    static constexpr size_t kMax = 50;

    // Adds an internal notification; returns it.
    const NotificationItem& AddInternal(std::wstring title, std::wstring body, TimeMs now);

    // Reconciles with the current list of Windows notifications. Items missing from `current`
    // were dismissed in Windows and are dropped here too. Returns the newly seen items.
    std::vector<NotificationItem> SyncWindows(const std::vector<NotificationItem>& current, TimeMs now);

    const std::deque<NotificationItem>& Items() const { return items_; }
    size_t UnseenCount() const { return unseen_; }
    void MarkAllSeen() { unseen_ = 0; }
    void RemoveLocal(uint64_t key); // hides from our list only (does not touch Windows)

private:
    void Trim();
    std::deque<NotificationItem> items_; // newest first
    uint64_t nextKey_ = 1;
    size_t unseen_ = 0;
    std::vector<uint32_t> dismissedWindowsIds_;
};

// Decides which transient boxes are visible. At most `maxVisible` at once; extra arrivals while
// boxes are full replace nothing and wait in a short queue; the queue is capped so a burst
// (or the end of a fullscreen session) never floods the screen.
class ToastPolicy {
public:
    struct Visible {
        NotificationItem item;
        TimeMs shownAt = 0;
        bool hovered = false;
    };

    ToastPolicy(size_t maxVisible = 2, size_t maxQueued = 2, int lifetimeMs = 5000)
        : maxVisible_(maxVisible), maxQueued_(maxQueued), lifetimeMs_(lifetimeMs) {}

    // `suppressed` = fullscreen, locked, or toasts disabled: item is not shown now or later.
    void Push(const NotificationItem& item, bool suppressed, TimeMs now);
    void Dismiss(uint64_t key);            // visual only
    void SetHovered(uint64_t key, bool h, TimeMs now); // hovering pauses expiry
    void Clear();                          // e.g. entering fullscreen
    bool Tick(TimeMs now);                 // expiry + promotion; true if visible set changed
    std::optional<TimeMs> NextDeadline() const;

    const std::vector<Visible>& VisibleItems() const { return visible_; }
    size_t QueuedCount() const { return queue_.size(); }

private:
    void Promote(TimeMs now);
    size_t maxVisible_, maxQueued_;
    int lifetimeMs_;
    std::vector<Visible> visible_;
    std::deque<NotificationItem> queue_;
};

} // namespace nb
