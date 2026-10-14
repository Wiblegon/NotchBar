#include "pch.h"

#include "gfx/Graphics.h"

#include "core/Log.h"

namespace nb::gfx {

Graphics& Graphics::Get() {
    static Graphics g;
    return g;
}

bool Graphics::Init() {
    D2D1_FACTORY_OPTIONS opts{};
#ifdef _DEBUG
    opts.debugLevel = D2D1_DEBUG_LEVEL_NONE;
#endif
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &opts, d2dFactory_.put_void()))) {
        LOGE("D2D factory failed");
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                   reinterpret_cast<IUnknown**>(dwrite_.put())))) {
        LOGE("DWrite factory failed");
        return false;
    }
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put()));
    return CreateDevice();
}

bool Graphics::CreateDevice() {
    d3d_ = nullptr;
    d2dDevice_ = nullptr;
    resourceDc_ = nullptr;
    dcomp_ = nullptr;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                  D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, d3d_.put(), nullptr, nullptr);
    if (FAILED(hr)) {
        LOGW("hardware D3D11 device failed (0x%08X), using WARP", static_cast<unsigned>(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               d3d_.put(), nullptr, nullptr);
        if (FAILED(hr)) return false;
    }
    auto dxgi = d3d_.as<IDXGIDevice>();
    if (FAILED(d2dFactory_->CreateDevice(dxgi.get(), d2dDevice_.put()))) return false;
    d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, resourceDc_.put());
    // Using the D2D device as rendering device lets IDCompositionSurface::BeginDraw hand out
    // ID2D1DeviceContext directly (no swap chains to manage per window).
    hr = DCompositionCreateDevice2(d2dDevice_.get(), __uuidof(IDCompositionDesktopDevice), dcomp_.put_void());
    if (FAILED(hr)) {
        LOGE("DComp device failed 0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    ++generation_;
    LOGI("graphics device created (generation %llu)", static_cast<unsigned long long>(generation_));
    return true;
}

bool Graphics::RecoverIfLost() {
    BOOL valid = TRUE;
    if (dcomp_) {
        winrt::com_ptr<IDCompositionDevice> dev;
        if (SUCCEEDED(dcomp_->QueryInterface(IID_PPV_ARGS(dev.put())))) dev->CheckDeviceState(&valid);
    }
    bool removed = !d3d_ || d3d_->GetDeviceRemovedReason() != S_OK || !valid;
    if (!removed) return false;
    LOGW("graphics device lost, recreating");
    if (!CreateDevice()) {
        LOGE("device recreation failed");
        return false;
    }
    for (auto& l : listeners_) l();
    return true;
}

void Graphics::Commit() {
    if (dcomp_ && FAILED(dcomp_->Commit())) RecoverIfLost();
}

void Graphics::Shutdown() {
    dcomp_ = nullptr;
    resourceDc_ = nullptr;
    d2dDevice_ = nullptr;
    d3d_ = nullptr;
}

} // namespace nb::gfx
