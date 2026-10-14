#include "model/Notifications.h"

#include <algorithm>

namespace nb {

const NotificationItem& NotificationHistory::AddInternal(std::wstring title, std::wstring body, TimeMs now) {
    NotificationItem n;
    n.key = nextKey_++;
    n.source = NotificationSource::Internal;
    n.appName = L"NotchBar";
    n.title = std::move(title);
    n.body = std::move(body);
    n.arrivedMs = now;
    items_.push_front(std::move(n));
    ++unseen_;
    Trim();
    return items_.front();
}

std::vector<NotificationItem> NotificationHistory::SyncWindows(const std::vector<NotificationItem>& current, TimeMs now) {
    std::vector<NotificationItem> fresh;
    // Drop Windows items that no longer exist in Windows.
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const NotificationItem& it) {
                                    if (it.source != NotificationSource::Windows) return false;
                                    return std::none_of(current.begin(), current.end(),
                                                        [&](const NotificationItem& c) { return c.windowsId == it.windowsId; });
                                }),
                 items_.end());
    for (const auto& c : current) {
        bool known = std::any_of(items_.begin(), items_.end(), [&](const NotificationItem& it) {
            return it.source == NotificationSource::Windows && it.windowsId == c.windowsId;
        });
        if (known) continue;
        if (std::find(dismissedWindowsIds_.begin(), dismissedWindowsIds_.end(), c.windowsId) != dismissedWindowsIds_.end())
            continue;
        NotificationItem n = c;
        n.key = nextKey_++;
        n.source = NotificationSource::Windows;
        if (n.arrivedMs == 0) n.arrivedMs = now;
        fresh.push_back(n);
    }
    // Newest last in `current` order -> insert so that newest ends up first.
    std::sort(fresh.begin(), fresh.end(), [](const auto& a, const auto& b) { return a.arrivedMs < b.arrivedMs; });
    for (const auto& n : fresh) items_.push_front(n);
    std::stable_sort(items_.begin(), items_.end(), [](const auto& a, const auto& b) { return a.arrivedMs > b.arrivedMs; });
    unseen_ += fresh.size();
    Trim();
    return fresh;
}

void NotificationHistory::RemoveLocal(uint64_t key) {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        if (it->key == key) {
            if (it->source == NotificationSource::Windows) dismissedWindowsIds_.push_back(it->windowsId);
            items_.erase(it);
            break;
        }
    }
    if (dismissedWindowsIds_.size() > 200) dismissedWindowsIds_.erase(dismissedWindowsIds_.begin(), dismissedWindowsIds_.begin() + 100);
}

void NotificationHistory::Trim() {
    while (items_.size() > kMax) items_.pop_back();
    unseen_ = std::min(unseen_, items_.size());
}

// ---------------------------------------------------------------------------------------------

void ToastPolicy::Push(const NotificationItem& item, bool suppressed, TimeMs now) {
    if (suppressed) return; // recorded in history by the caller; never replayed later
    queue_.push_back(item);
    while (queue_.size() > maxQueued_ + maxVisible_) queue_.pop_front(); // keep only the newest few
    Promote(now);
}

void ToastPolicy::Promote(TimeMs now) {
    while (visible_.size() < maxVisible_ && !queue_.empty()) {
        visible_.push_back({queue_.front(), now, false});
        queue_.pop_front();
    }
    while (queue_.size() > maxQueued_) queue_.pop_front();
}

void ToastPolicy::Dismiss(uint64_t key) {
    visible_.erase(std::remove_if(visible_.begin(), visible_.end(), [&](const Visible& v) { return v.item.key == key; }),
                   visible_.end());
}

void ToastPolicy::SetHovered(uint64_t key, bool h, TimeMs now) {
    for (auto& v : visible_)
        if (v.item.key == key) {
            v.hovered = h;
            if (!h) v.shownAt = now - lifetimeMs_ / 2; // give the reader some extra time after leaving
        }
}

void ToastPolicy::Clear() {
    visible_.clear();
    queue_.clear();
}

bool ToastPolicy::Tick(TimeMs now) {
    size_t before = visible_.size();
    visible_.erase(std::remove_if(visible_.begin(), visible_.end(),
                                  [&](const Visible& v) { return !v.hovered && now - v.shownAt >= lifetimeMs_; }),
                   visible_.end());
    bool changed = visible_.size() != before;
    size_t vb = visible_.size();
    Promote(now);
    return changed || visible_.size() != vb;
}

std::optional<TimeMs> ToastPolicy::NextDeadline() const {
    std::optional<TimeMs> d;
    for (const auto& v : visible_) {
        if (v.hovered) continue;
        TimeMs t = v.shownAt + lifetimeMs_;
        if (!d || t < *d) d = t;
    }
    return d;
}

} // namespace nb
