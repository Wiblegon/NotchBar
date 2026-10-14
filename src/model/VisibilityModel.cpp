#include "model/VisibilityModel.h"

namespace nb {

const char* ToString(ScreenMode m) {
    switch (m) {
    case ScreenMode::Desktop: return "desktop";
    case ScreenMode::Maximized: return "maximized";
    case ScreenMode::Fullscreen: return "fullscreen";
    }
    return "?";
}

const char* ToString(EdgePresence p) {
    switch (p) {
    case EdgePresence::Hidden: return "hidden";
    case EdgePresence::Collapsed: return "collapsed";
    case EdgePresence::Normal: return "normal";
    }
    return "?";
}

void EdgeBarModel::SetMode(ScreenMode m, TimeMs now) {
    if (m == mode_) return;
    mode_ = m;
    if (m == ScreenMode::Fullscreen) {
        // Popups are closed by the owner; a stale hover must not reopen anything later.
        pinned_ = false;
        hovered_ = false;
    }
    deadline_.reset();
    Recompute(now);
}

void EdgeBarModel::Recompute(TimeMs now) {
    switch (mode_) {
    case ScreenMode::Fullscreen:
        presence_ = EdgePresence::Hidden;
        deadline_.reset();
        break;
    case ScreenMode::Desktop:
        presence_ = EdgePresence::Normal;
        deadline_.reset();
        break;
    case ScreenMode::Maximized:
        if (pinned_) {
            presence_ = EdgePresence::Normal;
            deadline_.reset();
        } else if (presence_ == EdgePresence::Hidden) {
            // Coming back from fullscreen: start collapsed, never "peek" by itself.
            presence_ = EdgePresence::Collapsed;
            deadline_.reset();
        } else if (presence_ == EdgePresence::Normal && !hovered_) {
            // Mode switched while visible (e.g. a window got maximized): collapse after grace.
            pendingTarget_ = EdgePresence::Collapsed;
            deadline_ = now + t_.collapseDelayMs;
        }
        break;
    }
}

void EdgeBarModel::PointerEnter(TimeMs now) {
    if (mode_ == ScreenMode::Fullscreen) return; // edge touches never open anything in fullscreen
    hovered_ = true;
    if (mode_ != ScreenMode::Maximized) return;
    if (presence_ == EdgePresence::Normal) {
        deadline_.reset(); // cancel pending collapse
    } else if (presence_ == EdgePresence::Collapsed && !deadline_) {
        pendingTarget_ = EdgePresence::Normal;
        deadline_ = now + t_.expandDelayMs;
    }
}

void EdgeBarModel::PointerLeave(TimeMs now) {
    hovered_ = false;
    if (mode_ != ScreenMode::Maximized) return;
    if (presence_ == EdgePresence::Collapsed) {
        deadline_.reset(); // left before the dwell elapsed
    } else if (presence_ == EdgePresence::Normal && !pinned_) {
        pendingTarget_ = EdgePresence::Collapsed;
        deadline_ = now + t_.collapseDelayMs;
    }
}

void EdgeBarModel::SetPinned(bool pinned, TimeMs now) {
    if (pinned == pinned_) return;
    pinned_ = pinned;
    if (mode_ != ScreenMode::Maximized) return;
    if (pinned) {
        presence_ = EdgePresence::Normal;
        deadline_.reset();
    } else if (!hovered_) {
        pendingTarget_ = EdgePresence::Collapsed;
        deadline_ = now + t_.collapseDelayMs;
    }
}

bool EdgeBarModel::Tick(TimeMs now) {
    if (!deadline_ || now < *deadline_) return false;
    deadline_.reset();
    if (mode_ != ScreenMode::Maximized) return false;
    EdgePresence before = presence_;
    if (pendingTarget_ == EdgePresence::Normal && hovered_) presence_ = EdgePresence::Normal;
    else if (pendingTarget_ == EdgePresence::Collapsed && !hovered_ && !pinned_) presence_ = EdgePresence::Collapsed;
    return before != presence_;
}

// ---------------------------------------------------------------------------------------------

void HoverPanelModel::PointerEnterBar(PanelDir approachDir, TimeMs now) {
    switch (state_) {
    case State::Idle:
        dir_ = approachDir; // decided once per interaction
        state_ = State::OpenPending;
        deadline_ = now + t_.openDelayMs;
        break;
    case State::ClosePending:
        state_ = State::Open; // keep the locked direction
        deadline_.reset();
        break;
    case State::OpenPending:
    case State::Open:
        break;
    }
}

void HoverPanelModel::PointerEnterPanel(TimeMs) {
    if (state_ == State::ClosePending) {
        state_ = State::Open;
        deadline_.reset();
    }
}

void HoverPanelModel::PointerLeaveAll(TimeMs now) {
    if (state_ == State::OpenPending) {
        state_ = State::Idle;
        deadline_.reset();
    } else if (state_ == State::Open) {
        state_ = State::ClosePending;
        deadline_ = now + t_.closeDelayMs;
    }
}

void HoverPanelModel::OpenNow(PanelDir dir) {
    if (!IsOpen()) dir_ = dir;
    state_ = State::Open;
    deadline_.reset();
}

void HoverPanelModel::CloseNow() {
    state_ = State::Idle;
    deadline_.reset();
}

bool HoverPanelModel::Tick(TimeMs now) {
    if (!deadline_ || now < *deadline_) return false;
    deadline_.reset();
    if (state_ == State::OpenPending) {
        state_ = State::Open;
        return true;
    }
    if (state_ == State::ClosePending) {
        state_ = State::Idle;
        return true;
    }
    return false;
}

} // namespace nb
