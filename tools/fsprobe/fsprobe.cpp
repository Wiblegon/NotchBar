#define _CRT_SECURE_NO_WARNINGS
// fsprobe: borderless-fullscreen D3D11 "game stand-in" that records frame times.
// Not a real game. Used to (1) check that NotchBar classifies borderless fullscreen correctly and
// hides itself, and (2) compare frame pacing with NotchBar running vs. not running.
// Usage: fsprobe.exe [--seconds N] [--vsync] [--out results.txt]
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static bool g_quit = false;
LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_ESCAPE) g_quit = true;
    if (m == WM_DESTROY) g_quit = true;
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    double seconds = 20;
    bool vsync = false;
    std::wstring out = L"fsprobe_results.txt";
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) seconds = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"--vsync")) vsync = true;
        else if (!wcscmp(argv[i], L"--out") && i + 1 < argc) out = argv[++i];
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"fsprobe";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(mon, &mi);
    RECT r = mi.rcMonitor;
    // Borderless fullscreen: WS_POPUP covering the whole monitor (what most games use today).
    HWND hwnd = CreateWindowExW(0, L"fsprobe", L"fsprobe", WS_POPUP | WS_VISIBLE, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, inst, nullptr);
    SetForegroundWindow(hwnd);

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    IDXGIDevice* dxgiDev = nullptr;
    dev->QueryInterface(&dxgiDev);
    IDXGIAdapter* adapter = nullptr;
    dxgiDev->GetAdapter(&adapter);
    IDXGIFactory5* factory = nullptr;
    adapter->GetParent(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory));
    BOOL tearing = FALSE;
    if (factory) factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = r.right - r.left;
    sd.Height = r.bottom - r.top;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1* sc = nullptr;
    factory->CreateSwapChainForHwnd(dev, hwnd, &sd, nullptr, nullptr, &sc);
    ID3D11Texture2D* bb = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb));
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(bb, nullptr, &rtv);

    LARGE_INTEGER f, t0, prev, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    prev = t0;
    std::vector<double> ft;
    ft.reserve(200000);
    double elapsed = 0;
    while (!g_quit && elapsed < seconds) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        float c = static_cast<float>(0.5 + 0.5 * std::sin(elapsed * 2));
        float col[4] = {0.05f, 0.1f + 0.2f * c, 0.2f, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        for (int i = 0; i < 40; ++i) ctx->ClearRenderTargetView(rtv, col); // small, steady GPU load
        sc->Present(vsync ? 1 : 0, (!vsync && tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        QueryPerformanceCounter(&now);
        ft.push_back((now.QuadPart - prev.QuadPart) * 1000.0 / f.QuadPart);
        prev = now;
        elapsed = (now.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    }
    // Drop the first second (window/compositor warm-up).
    double skip = 0;
    size_t first = 0;
    while (first < ft.size() && skip < 1000) skip += ft[first++];
    std::vector<double> s(ft.begin() + first, ft.end());
    std::vector<double> sorted = s;
    std::sort(sorted.begin(), sorted.end());
    auto pct = [&](double p) { return sorted.empty() ? 0 : sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))]; };
    double sum = 0;
    for (double x : s) sum += x;
    double avg = s.empty() ? 0 : sum / s.size();
    // 1% low FPS = FPS of the average of the slowest 1% frames
    size_t n1 = std::max<size_t>(1, sorted.size() / 100);
    double slow = 0;
    for (size_t i = sorted.size() - n1; i < sorted.size(); ++i) slow += sorted[i];
    slow /= n1;
    size_t over = 0;
    for (double x : s)
        if (x > avg * 2) ++over;
    FILE* fo = _wfopen(out.c_str(), L"a");
    if (fo) {
        fwprintf(fo, L"mode=%s frames=%zu avg_ms=%.3f avg_fps=%.1f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f low1pct_fps=%.1f spikes_over_2x_avg=%zu\n",
                 vsync ? L"vsync" : L"uncapped", s.size(), avg, avg > 0 ? 1000 / avg : 0, pct(0.5), pct(0.95), pct(0.99),
                 sorted.empty() ? 0 : sorted.back(), slow > 0 ? 1000 / slow : 0, over);
        fclose(fo);
    }
    return 0;
}
