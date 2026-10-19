#include "pch.h"

#include "sys/AudioDevices.h"

#include <audiopolicy.h>
#include <devicetopology.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include "sys/WindowTracker.h"

namespace nb::sys {
namespace {

// The (undocumented but long-stable) interface behind Windows' own "set as default device".
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};
const CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

// Marks our own changes (same idea as AudioMonitor).
const GUID kCtx = {0x6b1f8a2e, 0x4c1d, 0x4a77, {0x9a, 0x51, 0x2e, 0x10, 0x7c, 0x3d, 0x55, 0x01}};

winrt::com_ptr<IMMDeviceEnumerator> Enumerator() {
    winrt::com_ptr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(e.put()));
    return e;
}

std::wstring FriendlyName(IMMDevice* d) {
    std::wstring r;
    winrt::com_ptr<IPropertyStore> ps;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, ps.put()))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) r = pv.pwszVal;
        PropVariantClear(&pv);
    }
    return r;
}

std::wstring DeviceId(IMMDevice* d) {
    LPWSTR id = nullptr;
    std::wstring r;
    if (SUCCEEDED(d->GetId(&id)) && id) r = id;
    CoTaskMemFree(id);
    return r;
}

winrt::com_ptr<IMMDevice> DefaultDevice(bool capture) {
    winrt::com_ptr<IMMDevice> d;
    if (auto e = Enumerator()) e->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, d.put());
    return d;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

// ---- driver effects through the device topology ------------------------------------------------
// Bass (render) = a part that offers IAudioBass; boost (capture) = a volume-level part whose name says so.
bool NameLooksLikeBoost(const std::wstring& n) {
    std::wstring l = Lower(n);
    return l.find(L"boost") != std::wstring::npos || l.find(L"takviye") != std::wstring::npos || l.find(L"artır") != std::wstring::npos ||
           l.find(L"güçlendir") != std::wstring::npos;
}

struct Effect {
    winrt::com_ptr<IPerChannelDbLevel> level;
    std::wstring name;
};

void Search(IPart* part, bool capture, std::set<std::wstring>& seen, std::optional<Effect>& out, int depth) {
    if (out || !part || depth > 16) return;
    LPWSTR gid = nullptr;
    if (SUCCEEDED(part->GetGlobalId(&gid)) && gid) {
        bool fresh = seen.insert(gid).second;
        CoTaskMemFree(gid);
        if (!fresh) return;
    }
    LPWSTR nm = nullptr;
    std::wstring name;
    if (SUCCEEDED(part->GetName(&nm)) && nm) name = nm;
    CoTaskMemFree(nm);
    if (!capture) {
        winrt::com_ptr<IAudioBass> bass;
        if (SUCCEEDED(part->Activate(CLSCTX_ALL, __uuidof(IAudioBass), bass.put_void()))) {
            out = Effect{bass.as<IPerChannelDbLevel>(), L"Bas"};
            return;
        }
    } else if (NameLooksLikeBoost(name)) {
        winrt::com_ptr<IAudioVolumeLevel> vol;
        if (SUCCEEDED(part->Activate(CLSCTX_ALL, __uuidof(IAudioVolumeLevel), vol.put_void()))) {
            out = Effect{vol.as<IPerChannelDbLevel>(), L"Güçlendirme"};
            return;
        }
    }
    for (int dir = 0; dir < 2 && !out; ++dir) {
        winrt::com_ptr<IPartsList> list;
        HRESULT hr = dir == 0 ? part->EnumPartsIncoming(list.put()) : part->EnumPartsOutgoing(list.put());
        if (FAILED(hr) || !list) continue;
        UINT n = 0;
        list->GetCount(&n);
        for (UINT i = 0; i < n && !out; ++i) {
            winrt::com_ptr<IPart> next;
            if (SUCCEEDED(list->GetPart(i, next.put()))) Search(next.get(), capture, seen, out, depth + 1);
        }
    }
}

std::optional<Effect> FindEffect(bool capture) {
    std::optional<Effect> out;
    auto dev = DefaultDevice(capture);
    if (!dev) return out;
    winrt::com_ptr<IDeviceTopology> topo;
    if (FAILED(dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, topo.put_void()))) return out;
    std::set<std::wstring> seen;
    UINT subs = 0;
    topo->GetSubunitCount(&subs);
    for (UINT i = 0; i < subs && !out; ++i) {
        winrt::com_ptr<ISubunit> su;
        if (FAILED(topo->GetSubunit(i, su.put()))) continue;
        if (auto part = su.try_as<IPart>()) Search(part.get(), capture, seen, out, 0);
    }
    return out;
}

} // namespace

std::vector<AudioDevice> ListAudioDevices(bool capture) {
    std::vector<AudioDevice> out;
    auto e = Enumerator();
    if (!e) return out;
    std::wstring def;
    if (auto d = DefaultDevice(capture)) def = DeviceId(d.get());
    winrt::com_ptr<IMMDeviceCollection> coll;
    if (FAILED(e->EnumAudioEndpoints(capture ? eCapture : eRender, DEVICE_STATE_ACTIVE, coll.put()))) return out;
    UINT n = 0;
    coll->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        winrt::com_ptr<IMMDevice> d;
        if (FAILED(coll->Item(i, d.put()))) continue;
        AudioDevice ad;
        ad.id = DeviceId(d.get());
        ad.name = FriendlyName(d.get());
        ad.isDefault = ad.id == def;
        out.push_back(std::move(ad));
    }
    return out;
}

bool SetDefaultAudioDevice(const std::wstring& id) {
    winrt::com_ptr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), pc.put_void()))) return false;
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) ok &= SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), role));
    return ok;
}

CaptureState GetCapture() {
    CaptureState s;
    auto dev = DefaultDevice(true);
    if (!dev) return s;
    winrt::com_ptr<IAudioEndpointVolume> v;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, v.put_void()))) {
        float l = 0;
        BOOL m = FALSE;
        v->GetMasterVolumeLevelScalar(&l);
        v->GetMute(&m);
        s.available = true;
        s.level = l;
        s.muted = m != FALSE;
    }
    s.name = FriendlyName(dev.get());
    return s;
}

void SetCaptureLevel(float level) {
    auto dev = DefaultDevice(true);
    winrt::com_ptr<IAudioEndpointVolume> v;
    if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, v.put_void()))) {
        level = std::clamp(level, 0.0f, 1.0f);
        v->SetMasterVolumeLevelScalar(level, &kCtx);
        if (level > 0) v->SetMute(FALSE, &kCtx);
    }
}

void SetCaptureMuted(bool muted) {
    auto dev = DefaultDevice(true);
    winrt::com_ptr<IAudioEndpointVolume> v;
    if (dev && SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, v.put_void())))
        v->SetMute(muted, &kCtx);
}

namespace {
struct SessionRef {
    winrt::com_ptr<ISimpleAudioVolume> vol;
    std::wstring id, name, exe;
};

std::vector<SessionRef> Sessions() {
    std::vector<SessionRef> out;
    auto dev = DefaultDevice(false);
    if (!dev) return out;
    winrt::com_ptr<IAudioSessionManager2> mgr;
    if (FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, mgr.put_void()))) return out;
    winrt::com_ptr<IAudioSessionEnumerator> en;
    if (FAILED(mgr->GetSessionEnumerator(en.put()))) return out;
    int n = 0;
    en->GetCount(&n);
    DWORD self = GetCurrentProcessId();
    for (int i = 0; i < n; ++i) {
        winrt::com_ptr<IAudioSessionControl> c;
        if (FAILED(en->GetSession(i, c.put()))) continue;
        auto c2 = c.try_as<IAudioSessionControl2>();
        auto vol = c.try_as<ISimpleAudioVolume>();
        if (!c2 || !vol) continue;
        AudioSessionState st{};
        c->GetState(&st);
        if (st == AudioSessionStateExpired) continue;
        DWORD pid = 0;
        c2->GetProcessId(&pid);
        if (pid == self) continue;
        SessionRef r;
        r.vol = vol;
        if (c2->IsSystemSoundsSession() == S_OK) {
            r.id = L"system";
            r.name = L"Sistem sesleri";
        } else {
            r.exe = WindowTracker::ProcessPath(pid);
            if (r.exe.empty()) continue;
            r.id = Lower(r.exe);
            std::wstring stem = PathFindFileNameW(r.exe.c_str());
            if (auto dot = stem.rfind(L'.'); dot != std::wstring::npos) stem.resize(dot);
            if (!stem.empty()) stem[0] = static_cast<wchar_t>(::towupper(stem[0]));
            r.name = stem;
        }
        out.push_back(std::move(r));
    }
    return out;
}
} // namespace

std::vector<AppVolume> ListAppVolumes() {
    std::vector<AppVolume> out;
    for (auto& s : Sessions()) {
        if (std::any_of(out.begin(), out.end(), [&](const AppVolume& a) { return a.id == s.id; })) continue;
        AppVolume a;
        a.id = s.id;
        a.name = s.name;
        a.exePath = s.exe;
        float l = 1;
        BOOL m = FALSE;
        s.vol->GetMasterVolume(&l);
        s.vol->GetMute(&m);
        a.level = l;
        a.muted = m != FALSE;
        out.push_back(std::move(a));
    }
    return out;
}

void SetAppVolume(const std::wstring& id, float level) {
    for (auto& s : Sessions())
        if (s.id == id) {
            s.vol->SetMasterVolume(std::clamp(level, 0.0f, 1.0f), &kCtx);
            if (level > 0) s.vol->SetMute(FALSE, &kCtx);
        }
}

void SetAppMuted(const std::wstring& id, bool muted) {
    for (auto& s : Sessions())
        if (s.id == id) s.vol->SetMute(muted, &kCtx);
}

std::optional<DeviceEffect> GetEffect(bool capture) {
    auto fx = FindEffect(capture);
    if (!fx) return std::nullopt;
    UINT ch = 0;
    fx->level->GetChannelCount(&ch);
    if (!ch) return std::nullopt;
    float lo = 0, hi = 0, step = 0, cur = 0;
    if (FAILED(fx->level->GetLevelRange(0, &lo, &hi, &step)) || hi <= lo) return std::nullopt;
    fx->level->GetLevel(0, &cur);
    return DeviceEffect{fx->name, std::clamp((cur - lo) / (hi - lo), 0.0f, 1.0f)};
}

void SetEffect(bool capture, float value) {
    auto fx = FindEffect(capture);
    if (!fx) return;
    UINT ch = 0;
    fx->level->GetChannelCount(&ch);
    float lo = 0, hi = 0, step = 0;
    if (FAILED(fx->level->GetLevelRange(0, &lo, &hi, &step)) || hi <= lo) return;
    float db = lo + std::clamp(value, 0.0f, 1.0f) * (hi - lo);
    for (UINT c = 0; c < ch; ++c) fx->level->SetLevel(c, db, &kCtx);
}

} // namespace nb::sys
