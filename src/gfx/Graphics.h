#pragma once
// Process-wide graphics: one D3D11 device, one D2D device and one DirectComposition desktop
// device shared by every surface. Recreated as a whole when the GPU device is lost.
#include <dcomp.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <winrt/base.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace nb::gfx {

class Graphics {
public:
    static Graphics& Get();

    bool Init();
    void Shutdown();

    ID2D1Factory3* D2D() const { return d2dFactory_.get(); }
    IDWriteFactory3* DWrite() const { return dwrite_.get(); }
    IWICImagingFactory* Wic() const { return wic_.get(); }
    ID2D1Device2* D2DDevice() const { return d2dDevice_.get(); }
    ID2D1DeviceContext* ResourceContext() const { return resourceDc_.get(); } // for creating bitmaps
    IDCompositionDesktopDevice* DComp() const { return dcomp_.get(); }
    uint64_t Generation() const { return generation_; }

    // Called after a failed draw/commit. Recreates device objects if the device is gone.
    // Returns true if a recreation happened (callers must rebuild device-dependent state).
    bool RecoverIfLost();
    void Commit();
    void OnDeviceRecreated(std::function<void()> fn) { listeners_.push_back(std::move(fn)); }

private:
    bool CreateDevice();
    winrt::com_ptr<ID2D1Factory3> d2dFactory_;
    winrt::com_ptr<IDWriteFactory3> dwrite_;
    winrt::com_ptr<IWICImagingFactory> wic_;
    winrt::com_ptr<ID3D11Device> d3d_;
    winrt::com_ptr<ID2D1Device2> d2dDevice_;
    winrt::com_ptr<ID2D1DeviceContext> resourceDc_;
    winrt::com_ptr<IDCompositionDesktopDevice> dcomp_;
    uint64_t generation_ = 0;
    std::vector<std::function<void()>> listeners_;
};

} // namespace nb::gfx
