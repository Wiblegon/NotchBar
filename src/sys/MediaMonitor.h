#pragma once
// System media sessions (Windows.Media.Control, Windows 10 1809+): Spotify and any other app
// that publishes System Media Transport Controls. Event-driven; the album art is only fetched
// while the media box is open.
#include <d2d1.h>

#include <functional>
#include <string>
#include <vector>

#include "model/VisibilityModel.h"

namespace nb::sys {

struct MediaState {
    bool hasSession = false;
    std::wstring appId;    // SourceAppUserModelId
    std::wstring appName;
    std::wstring iconKey;  // IconCache key for the source app
    std::wstring title, artist, album;
    bool playing = false;
    bool canPlayPause = false, canNext = false, canPrev = false;
    bool hasTimeline = false;
    double positionSec = 0, durationSec = 0;
    TimeMs positionAtMs = 0; // UI clock time when positionSec was valid
};

class MediaMonitor {
public:
    static MediaMonitor& Get();
    void Start();
    void Stop();
    const MediaState& State() const { return state_; }
    void Subscribe(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

    void PlayPause();
    void Next();
    void Previous();

    // Album art for the current track, decoded to premultiplied BGRA (nullptr until loaded).
    // Requesting it starts the load; ReleaseArt() frees it when the box closes.
    ID2D1Bitmap* Art();
    void ReleaseArt();
    double CurrentPosition(TimeMs now) const;

    // internal
    void Apply(MediaState s, bool trackChanged);
    void ArtLoaded(std::vector<uint8_t> px, unsigned w, unsigned h, uint64_t token);

private:
    MediaState state_;
    std::vector<std::function<void()>> listeners_;
};

} // namespace nb::sys
