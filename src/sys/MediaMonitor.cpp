#include "pch.h"

#include "sys/MediaMonitor.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <shcore.h>

#include "core/Dispatcher.h"
#include "core/Log.h"
#include "gfx/Graphics.h"
#include "sys/AppCatalog.h"
#include "sys/WindowTracker.h"

namespace nb::sys {
namespace {
using namespace winrt;
using namespace winrt::Windows::Media::Control;
using winrt::Windows::Foundation::TimeSpan;

GlobalSystemMediaTransportControlsSessionManager g_mgr{nullptr};
GlobalSystemMediaTransportControlsSession g_session{nullptr}; // the one shown / controlled
event_token g_tokCurrent{}, g_tokSessions{};

struct Bound {
    GlobalSystemMediaTransportControlsSession s{nullptr};
    event_token props{}, playback{}, timeline{};
    bool hasTimeline = false;
};
std::vector<Bound> g_all; // every session with its event subscriptions
std::wstring g_pref;      // app the user picked in the media box (empty = automatic)
std::atomic<uint64_t> g_refreshGen{0};

winrt::com_ptr<ID2D1Bitmap> g_art;
std::vector<uint8_t> g_artPixels;
unsigned g_artW = 0, g_artH = 0;
uint64_t g_artToken = 0, g_artGen = 0;
bool g_artLoading = false;
std::wstring g_artTrack;

std::wstring IconKeyFor(const std::wstring& aumid) {
    if (aumid.empty()) return {};
    if (AppCatalog::Get().Find(aumid)) return L"app:" + aumid;
    // Desktop Spotify reports "Spotify.exe": find a running window of that executable.
    std::wstring lower = aumid;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    for (const auto& g : WindowTracker::Get().Groups()) {
        std::wstring exe = g.exePath;
        std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);
        if (!exe.empty() && exe.ends_with(L"\\" + lower)) return L"file:" + g.exePath;
        if (g.appId == aumid) return g.iconKey;
    }
    if (lower.ends_with(L".exe")) {
        for (const auto& e : AppCatalog::Get().Apps())
            if (e.target.ends_with(L"\\" + lower)) return e.IconKey();
    }
    return L"app:" + aumid;
}

std::wstring NameFor(const std::wstring& aumid) {
    if (const auto* e = AppCatalog::Get().Find(aumid)) return e->name;
    std::wstring n = aumid;
    if (auto bang = n.find(L'!'); bang != std::wstring::npos) n = n.substr(0, bang);
    if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".exe") == 0) n.resize(n.size() - 4);
    if (auto us = n.find(L'_'); us != std::wstring::npos) n = n.substr(0, us);
    if (auto dot = n.rfind(L'.'); dot != std::wstring::npos && dot + 1 < n.size()) n = n.substr(dot + 1);
    return n;
}

std::wstring IdOf(const GlobalSystemMediaTransportControlsSession& s) {
    try {
        return s ? std::wstring(s.SourceAppUserModelId().c_str()) : std::wstring();
    } catch (...) {
        return {};
    }
}

bool IsPlaying(const GlobalSystemMediaTransportControlsSession& s) {
    try {
        auto info = s ? s.GetPlaybackInfo() : nullptr;
        return info && info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    } catch (...) {
        return false;
    }
}

fire_and_forget RefreshAsync() {
    uint64_t gen = ++g_refreshGen;
    auto session = g_session;
    std::vector<GlobalSystemMediaTransportControlsSession> all;
    for (const auto& b : g_all) all.push_back(b.s);
    MediaState s;
    bool trackChanged = false;
    for (const auto& one : all) {
        MediaSource src;
        src.appId = IdOf(one);
        src.playing = IsPlaying(one);
        src.selected = session && src.appId == IdOf(session);
        s.sources.push_back(std::move(src));
    }
    if (session) {
        try {
            co_await resume_background();
            s.hasSession = true;
            s.appId = session.SourceAppUserModelId().c_str();
            auto props = co_await session.TryGetMediaPropertiesAsync();
            if (props) {
                s.title = props.Title().c_str();
                s.artist = props.Artist().c_str();
                s.album = props.AlbumTitle().c_str();
            }
            auto info = session.GetPlaybackInfo();
            if (info) {
                s.playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                auto c = info.Controls();
                s.canPlayPause = c.IsPlayPauseToggleEnabled() || c.IsPlayEnabled() || c.IsPauseEnabled();
                s.canNext = c.IsNextEnabled();
                s.canPrev = c.IsPreviousEnabled();
            }
            auto tl = session.GetTimelineProperties();
            if (tl) {
                double end = std::chrono::duration<double>(tl.EndTime() - tl.StartTime()).count();
                double pos = std::chrono::duration<double>(tl.Position() - tl.StartTime()).count();
                if (end > 0.5 && pos >= 0) {
                    s.hasTimeline = true;
                    s.durationSec = end;
                    // Extrapolate from when the app last reported the position.
                    auto updated = winrt::clock::to_file_time(tl.LastUpdatedTime());
                    FILETIME now;
                    GetSystemTimeAsFileTime(&now);
                    int64_t ageMs = ((static_cast<int64_t>(now.dwHighDateTime) << 32 | now.dwLowDateTime) - static_cast<int64_t>(updated.value)) / 10000;
                    if (s.playing && ageMs > 0 && ageMs < 24 * 3600 * 1000) pos += ageMs / 1000.0;
                    s.positionSec = std::min(pos, end);
                }
            }
        } catch (...) {
            auto keep = std::move(s.sources);
            s = {};
            s.sources = std::move(keep);
            s.hasSession = static_cast<bool>(session);
        }
    }
    ui::Post([s = std::move(s), gen]() mutable {
        if (gen != g_refreshGen) return; // a newer refresh superseded this one
        s.positionAtMs = ui::NowMs();
        s.appName = NameFor(s.appId);
        s.iconKey = IconKeyFor(s.appId);
        for (auto& src : s.sources) {
            src.name = NameFor(src.appId);
            src.iconKey = IconKeyFor(src.appId);
        }
        const auto& old = MediaMonitor::Get().State();
        bool changed = old.title != s.title || old.artist != s.artist || old.appId != s.appId;
        MediaMonitor::Get().Apply(std::move(s), changed);
    });
    (void)trackChanged;
}

void Unbind() {
    for (auto& b : g_all) {
        try {
            b.s.MediaPropertiesChanged(b.props);
            b.s.PlaybackInfoChanged(b.playback);
            if (b.hasTimeline) b.s.TimelinePropertiesChanged(b.timeline);
        } catch (...) {
        }
    }
    g_all.clear();
    g_session = nullptr;
}

void Rebind() {
    std::wstring prevId = IdOf(g_session);
    Unbind();
    if (!g_mgr) {
        RefreshAsync();
        return;
    }
    try {
        for (auto s : g_mgr.GetSessions()) {
            Bound b;
            b.s = s;
            b.props = s.MediaPropertiesChanged([](auto&&, auto&&) { ui::Post([] { RefreshAsync(); }); });
            // A play/pause anywhere may change which app should be shown.
            b.playback = s.PlaybackInfoChanged([](auto&&, auto&&) { ui::Post([] { Rebind(); }); });
            g_all.push_back(std::move(b));
        }
    } catch (...) {
    }
    // Choice: the app the user picked > the one already shown while it still plays > Windows' current
    // session if it plays > any playing one > the current / first session.
    Bound* chosen = nullptr;
    auto find = [&](const std::wstring& id) -> Bound* {
        if (id.empty()) return nullptr;
        for (auto& b : g_all)
            if (IdOf(b.s) == id) return &b;
        return nullptr;
    };
    if (!g_pref.empty()) {
        chosen = find(g_pref);
        if (!chosen) g_pref.clear(); // that app closed: back to automatic
    }
    std::wstring currentId;
    try {
        currentId = IdOf(g_mgr.GetCurrentSession());
    } catch (...) {
    }
    if (!chosen) {
        if (auto* p = find(prevId); p && IsPlaying(p->s)) chosen = p;
    }
    if (!chosen) {
        if (auto* p = find(currentId); p && IsPlaying(p->s)) chosen = p;
    }
    if (!chosen) {
        for (auto& b : g_all)
            if (IsPlaying(b.s)) {
                chosen = &b;
                break;
            }
    }
    if (!chosen) chosen = find(currentId);
    if (!chosen && !g_all.empty()) chosen = &g_all.front();
    if (chosen) {
        g_session = chosen->s;
        try { // the session may already be gone: never let a dead session take the app down
            chosen->timeline = chosen->s.TimelinePropertiesChanged([](auto&&, auto&&) { ui::Post([] { RefreshAsync(); }); });
            chosen->hasTimeline = true;
        } catch (...) {
            chosen->hasTimeline = false;
        }
    }
    RefreshAsync();
}

fire_and_forget LoadArtAsync(GlobalSystemMediaTransportControlsSession session, uint64_t token) {
    std::vector<uint8_t> px;
    unsigned w = 0, h = 0;
    try {
        co_await resume_background();
        auto props = co_await session.TryGetMediaPropertiesAsync();
        auto thumb = props ? props.Thumbnail() : nullptr;
        if (thumb) {
            auto stream = co_await thumb.OpenReadAsync();
            winrt::com_ptr<IStream> istream;
            if (SUCCEEDED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(istream.put())))) {
                winrt::com_ptr<IWICImagingFactory> wic;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));
                winrt::com_ptr<IWICBitmapDecoder> dec;
                if (wic && SUCCEEDED(wic->CreateDecoderFromStream(istream.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) {
                    winrt::com_ptr<IWICBitmapFrameDecode> frame;
                    dec->GetFrame(0, frame.put());
                    UINT sw = 0, sh = 0;
                    frame->GetSize(&sw, &sh);
                    // Bounded: the box shows art at ~64 DIP; never keep large images around.
                    UINT target = 160;
                    winrt::com_ptr<IWICBitmapSource> src = frame;
                    if (sw > target || sh > target) {
                        winrt::com_ptr<IWICBitmapScaler> sc;
                        wic->CreateBitmapScaler(sc.put());
                        float k = static_cast<float>(target) / std::max(sw, sh);
                        sc->Initialize(frame.get(), std::max(1u, static_cast<UINT>(sw * k)), std::max(1u, static_cast<UINT>(sh * k)),
                                       WICBitmapInterpolationModeFant);
                        src = sc;
                    }
                    winrt::com_ptr<IWICBitmapSource> conv;
                    if (SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, src.get(), conv.put()))) {
                        conv->GetSize(&w, &h);
                        px.resize(static_cast<size_t>(w) * h * 4);
                        if (FAILED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size()), px.data()))) px.clear();
                    }
                }
            }
        }
    } catch (...) {
        px.clear();
    }
    ui::Post([px = std::move(px), w, h, token]() mutable { MediaMonitor::Get().ArtLoaded(std::move(px), w, h, token); });
}
} // namespace

MediaMonitor& MediaMonitor::Get() {
    static MediaMonitor m;
    return m;
}

void MediaMonitor::Start() {
    [](MediaMonitor*) -> fire_and_forget {
        try {
            auto mgr = co_await GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            ui::Post([mgr] {
                g_mgr = mgr;
                g_tokCurrent = g_mgr.CurrentSessionChanged([](auto&&, auto&&) { ui::Post([] { Rebind(); }); });
                g_tokSessions = g_mgr.SessionsChanged([](auto&&, auto&&) { ui::Post([] { Rebind(); }); });
                Rebind();
                LOGI("media: session manager ready");
            });
        } catch (...) {
            LOGW("media: session manager unavailable");
        }
    }(this);
}

void MediaMonitor::Stop() {
    Unbind();
    try {
        if (g_mgr) {
            g_mgr.CurrentSessionChanged(g_tokCurrent);
            g_mgr.SessionsChanged(g_tokSessions);
        }
    } catch (...) {
    }
    g_mgr = nullptr;
    ReleaseArt();
}

void MediaMonitor::Apply(MediaState s, bool trackChanged) {
    state_ = std::move(s);
    if (trackChanged) {
        // New track: drop the old art; it is reloaded only if the box is open.
        bool wanted = g_art != nullptr || g_artLoading;
        ReleaseArt();
        if (wanted) Art();
    }
    for (auto& l : listeners_) l();
}

double MediaMonitor::CurrentPosition(TimeMs now) const {
    if (!state_.hasTimeline) return 0;
    double p = state_.positionSec;
    if (state_.playing) p += (now - state_.positionAtMs) / 1000.0;
    return std::clamp(p, 0.0, state_.durationSec);
}

ID2D1Bitmap* MediaMonitor::Art() {
    auto& g = gfx::Graphics::Get();
    if (!g_artPixels.empty()) {
        if (!g_art || g_artGen != g.Generation()) {
            g_art = nullptr;
            auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            g.ResourceContext()->CreateBitmap({g_artW, g_artH}, g_artPixels.data(), g_artW * 4, props, g_art.put());
            g_artGen = g.Generation();
        }
        return g_art.get();
    }
    if (!g_artLoading && g_session && state_.hasSession) {
        g_artLoading = true;
        LoadArtAsync(g_session, ++g_artToken);
    }
    return nullptr;
}

void MediaMonitor::ArtLoaded(std::vector<uint8_t> px, unsigned w, unsigned h, uint64_t token) {
    if (token != g_artToken || !g_artLoading) return; // stale (track changed or box closed)
    g_artLoading = false;
    g_artPixels = std::move(px);
    g_artW = w;
    g_artH = h;
    g_art = nullptr;
    for (auto& l : listeners_) l();
}

void MediaMonitor::ReleaseArt() {
    g_art = nullptr;
    g_artPixels.clear();
    g_artPixels.shrink_to_fit();
    g_artLoading = false;
    ++g_artToken;
}

void MediaMonitor::SelectSource(const std::wstring& appId) {
    g_pref = appId;
    Rebind();
}

void MediaMonitor::PlayPause() {
    if (g_session) g_session.TryTogglePlayPauseAsync();
}
void MediaMonitor::Next() {
    if (g_session) g_session.TrySkipNextAsync();
}
void MediaMonitor::Previous() {
    if (g_session) g_session.TrySkipPreviousAsync();
}

} // namespace nb::sys
