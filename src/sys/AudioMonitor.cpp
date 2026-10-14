#include "pch.h"

#include "sys/AudioMonitor.h"

#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include "core/Dispatcher.h"
#include "core/Log.h"

namespace nb::sys {
namespace {
// Our own changes carry this context so they are recognisable in callbacks.
const GUID kOwnContext = {0x6b1f8a2e, 0x4c1d, 0x4a77, {0x9a, 0x51, 0x2e, 0x10, 0x7c, 0x3d, 0x55, 0x01}};

struct VolumeCallback : winrt::implements<VolumeCallback, IAudioEndpointVolumeCallback> {
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (!d) return S_OK;
        float level = d->fMasterVolume;
        bool muted = d->bMuted != FALSE;
        ui::Post([level, muted] { AudioMonitor::Get().Apply(level, muted); });
        return S_OK;
    }
};

struct DeviceCallback : winrt::implements<DeviceCallback, IMMNotificationClient> {
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) ui::Post([] { AudioMonitor::Get().Rebind(); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        ui::Post([] { AudioMonitor::Get().Rebind(); });
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

winrt::com_ptr<IMMDeviceEnumerator> g_enum;
winrt::com_ptr<IAudioEndpointVolume> g_vol;
winrt::com_ptr<IAudioEndpointVolumeCallback> g_volCb;
winrt::com_ptr<IMMNotificationClient> g_devCb;
} // namespace

AudioMonitor& AudioMonitor::Get() {
    static AudioMonitor a;
    return a;
}

void AudioMonitor::Start() {
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(g_enum.put())))) {
        LOGE("audio: no device enumerator");
        return;
    }
    g_devCb = winrt::make<DeviceCallback>();
    g_enum->RegisterEndpointNotificationCallback(g_devCb.get());
    Rebind();
}

void AudioMonitor::Stop() {
    if (g_vol && g_volCb) g_vol->UnregisterControlChangeNotify(g_volCb.get());
    if (g_enum && g_devCb) g_enum->UnregisterEndpointNotificationCallback(g_devCb.get());
    g_vol = nullptr;
    g_volCb = nullptr;
    g_devCb = nullptr;
    g_enum = nullptr;
}

void AudioMonitor::Rebind() {
    if (g_vol && g_volCb) g_vol->UnregisterControlChangeNotify(g_volCb.get());
    g_vol = nullptr;
    state_ = {};
    winrt::com_ptr<IMMDevice> dev;
    if (g_enum && SUCCEEDED(g_enum->GetDefaultAudioEndpoint(eRender, eConsole, dev.put())) && dev) {
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, g_vol.put_void()))) {
            if (!g_volCb) g_volCb = winrt::make<VolumeCallback>();
            g_vol->RegisterControlChangeNotify(g_volCb.get());
            float l = 0;
            BOOL m = FALSE;
            g_vol->GetMasterVolumeLevelScalar(&l);
            g_vol->GetMute(&m);
            state_.available = true;
            state_.level = l;
            state_.muted = m != FALSE;
        }
        winrt::com_ptr<IPropertyStore> ps;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, ps.put()))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) state_.deviceName = pv.pwszVal;
            PropVariantClear(&pv);
        }
    }
    LOGI("audio: default output %s", state_.available ? "bound" : "missing");
    for (auto& l : listeners_) l();
}

void AudioMonitor::Apply(float level, bool muted) {
    if (level == state_.level && muted == state_.muted) return;
    state_.level = level;
    state_.muted = muted;
    for (auto& l : listeners_) l();
}

void AudioMonitor::SetLevel(float level) {
    if (!g_vol) return;
    level = std::clamp(level, 0.0f, 1.0f);
    g_vol->SetMasterVolumeLevelScalar(level, &kOwnContext);
    if (state_.muted && level > 0) g_vol->SetMute(FALSE, &kOwnContext);
    Apply(level, state_.muted && level <= 0);
}

void AudioMonitor::SetMuted(bool muted) {
    if (!g_vol) return;
    g_vol->SetMute(muted, &kOwnContext);
    Apply(state_.level, muted);
}

} // namespace nb::sys
