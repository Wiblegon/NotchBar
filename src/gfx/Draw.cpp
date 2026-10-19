#include "pch.h"

#include "gfx/Draw.h"

#include <d2d1effects.h>

#include "gfx/Anim.h"
#include "gfx/Graphics.h"
#include "gfx/Theme.h"

namespace nb::gfx {
namespace {
std::atomic<bool> g_reduced{false};
}
bool ReducedMotion() { return g_reduced.load(); }
void SetReducedMotion(bool r) { g_reduced = r; }
} // namespace nb::gfx

namespace nb::draw {
namespace {
std::wstring g_textFont = L"Segoe UI";
std::wstring g_iconFont = L"Segoe MDL2 Assets";

struct FormatKey {
    float size;
    int weight;
    bool icon;
    int align;
    bool operator<(const FormatKey& o) const {
        return std::tie(size, weight, icon, align) < std::tie(o.size, o.weight, o.icon, o.align);
    }
};
std::map<FormatKey, winrt::com_ptr<IDWriteTextFormat>> g_formats;

bool FontExists(const wchar_t* family) {
    winrt::com_ptr<IDWriteFontCollection> fc;
    gfx::Graphics::Get().DWrite()->GetSystemFontCollection(fc.put());
    UINT32 idx;
    BOOL exists = FALSE;
    fc->FindFamilyName(family, &idx, &exists);
    return exists;
}

IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT w, bool icon, Align align) {
    FormatKey k{size, static_cast<int>(w), icon, static_cast<int>(align)};
    auto it = g_formats.find(k);
    if (it != g_formats.end()) return it->second.get();
    winrt::com_ptr<IDWriteTextFormat> f;
    gfx::Graphics::Get().DWrite()->CreateTextFormat(icon ? g_iconFont.c_str() : g_textFont.c_str(), nullptr, w,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"tr-TR",
                                                    f.put());
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetTextAlignment(align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : align == Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                   : DWRITE_TEXT_ALIGNMENT_LEADING);
    if (!icon) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        winrt::com_ptr<IDWriteInlineObject> ellipsis;
        gfx::Graphics::Get().DWrite()->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
        f->SetTrimming(&trim, ellipsis.get());
    }
    auto* raw = f.get();
    g_formats[k] = std::move(f);
    return raw;
}

struct BrushCache {
    ID2D1DeviceContext* dc = nullptr;
    winrt::com_ptr<ID2D1SolidColorBrush> brush;
} g_brush;
} // namespace

void Init() {
    if (FontExists(L"Segoe UI Variable Text")) g_textFont = L"Segoe UI Variable Text";
    if (FontExists(L"Segoe Fluent Icons")) g_iconFont = L"Segoe Fluent Icons";
}

wchar_t BatteryGlyph(int p, bool charging) {
    int step = std::clamp((p + 5) / 10, 0, 10);
    if (charging) return step >= 10 ? glyph::BatteryCharging10 : static_cast<wchar_t>(glyph::BatteryCharging0 + step);
    return step >= 10 ? glyph::Battery10 : static_cast<wchar_t>(glyph::Battery0 + step);
}

wchar_t VolumeGlyph(float level, bool muted) {
    if (muted) return glyph::Mute;
    if (level <= 0.001f) return glyph::Volume0;
    if (level < 0.34f) return glyph::Volume1;
    if (level < 0.67f) return glyph::Volume2;
    return glyph::Volume3;
}

ID2D1SolidColorBrush* Brush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c) {
    // DComp hands out the same device context per device; recreate the brush if it changes.
    if (g_brush.dc != dc || !g_brush.brush) {
        g_brush.brush = nullptr;
        dc->CreateSolidColorBrush(c, g_brush.brush.put());
        g_brush.dc = dc;
    } else {
        g_brush.brush->SetColor(c);
    }
    return g_brush.brush.get();
}

void Text(ID2D1DeviceContext* dc, std::wstring_view text, const D2D1_RECT_F& r, float size, const D2D1_COLOR_F& c,
          DWRITE_FONT_WEIGHT weight, Align align, bool tabular) {
    if (text.empty() || c.a <= 0) return;
    auto* fmt = Format(size, weight, false, align);
    winrt::com_ptr<IDWriteTextLayout> layout;
    gfx::Graphics::Get().DWrite()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), fmt,
                                                    std::max(0.0f, r.right - r.left), std::max(0.0f, r.bottom - r.top),
                                                    layout.put());
    if (!layout) return;
    if (tabular) {
        winrt::com_ptr<IDWriteTypography> typo;
        gfx::Graphics::Get().DWrite()->CreateTypography(typo.put());
        typo->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
        layout->SetTypography(typo.get(), {0, static_cast<UINT32>(text.size())});
    }
    dc->DrawTextLayout({r.left, r.top}, layout.get(), Brush(dc, c), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float TextWidth(std::wstring_view text, float size, DWRITE_FONT_WEIGHT weight) {
    if (text.empty()) return 0;
    winrt::com_ptr<IDWriteTextLayout> layout;
    gfx::Graphics::Get().DWrite()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()),
                                                    Format(size, weight, false, Align::Leading), 10000, 100, layout.put());
    DWRITE_TEXT_METRICS m{};
    if (layout) layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void Glyph(ID2D1DeviceContext* dc, wchar_t g, D2D1_POINT_2F c, float size, const D2D1_COLOR_F& color) {
    if (color.a <= 0) return;
    auto* fmt = Format(size, DWRITE_FONT_WEIGHT_NORMAL, true, Align::Center);
    D2D1_RECT_F r{c.x - size, c.y - size, c.x + size, c.y + size};
    dc->DrawTextW(&g, 1, fmt, r, Brush(dc, color), D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
}

void RoundRect(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& fill) {
    dc->FillRoundedRectangle({r, radius, radius}, Brush(dc, fill));
}

void Panel(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, float opacity, bool shadow) {
    if (opacity <= 0.001f) return;
    if (shadow) {
        // Small, controlled shadow: a few soft expanding rings with low alpha.
        for (int i = 4; i >= 1; --i) {
            float g = i * 2.0f;
            D2D1_RECT_F s{r.left - g * 0.5f, r.top - g * 0.2f, r.right + g * 0.5f, r.bottom + g};
            dc->FillRoundedRectangle({s, radius + g * 0.6f, radius + g * 0.6f},
                                     Brush(dc, D2D1::ColorF(0, 0, 0, 0.055f * opacity)));
        }
    }
    auto fill = theme::kSurface;
    fill.a *= opacity;
    dc->FillRoundedRectangle({r, radius, radius}, Brush(dc, fill));
    auto edge = theme::kEdge;
    edge.a *= opacity;
    D2D1_RECT_F inner{r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f};
    dc->DrawRoundedRectangle({inner, radius - 0.5f, radius - 0.5f}, Brush(dc, edge), 1.0f);
}

void FocusRing(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius) {
    dc->DrawRoundedRectangle({r, radius, radius}, Brush(dc, theme::kFocus), 1.5f);
}

winrt::com_ptr<ID2D1PathGeometry> NotchPath(float x0, float w, float h, float flare, float radius) {
    winrt::com_ptr<ID2D1PathGeometry> path;
    gfx::Graphics::Get().D2D()->CreatePathGeometry(path.put());
    winrt::com_ptr<ID2D1GeometrySink> sink;
    path->Open(sink.put());
    radius = std::min(radius, std::min(h, w / 2));
    flare = std::min(flare, h);
    float x1 = x0 + w;
    sink->BeginFigure({x0 - flare, 0}, D2D1_FIGURE_BEGIN_FILLED);
    // concave flare into the left side
    sink->AddQuadraticBezier({{x0, 0}, {x0, flare}});
    sink->AddLine({x0, h - radius});
    sink->AddArc({{x0 + radius, h}, {radius, radius}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    sink->AddLine({x1 - radius, h});
    sink->AddArc({{x1, h - radius}, {radius, radius}, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
    sink->AddLine({x1, flare});
    sink->AddQuadraticBezier({{x1, 0}, {x1 + flare, 0}});
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    return path;
}

void Bitmap(ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float opacity) {
    if (!bmp) return;
    dc->DrawBitmap(bmp, r, opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
}

void BitmapMono(ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float opacity) {
    if (!bmp) return;
    winrt::com_ptr<ID2D1Effect> fx;
    if (FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, fx.put()))) {
        Bitmap(dc, bmp, r, opacity);
        return;
    }
    fx->SetInput(0, bmp);
    // luminance * 0.5 + 0.45: black icons stay readable, white ones do not glare
    D2D1_MATRIX_5X4_F m{0.30f * 0.5f, 0.30f * 0.5f, 0.30f * 0.5f, 0, 0.59f * 0.5f, 0.59f * 0.5f, 0.59f * 0.5f, 0, 0.11f * 0.5f,
                        0.11f * 0.5f, 0.11f * 0.5f, 0, 0, 0, 0, opacity, 0.45f, 0.45f, 0.45f, 0};
    fx->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
    winrt::com_ptr<ID2D1Image> img;
    fx->GetOutput(img.put());
    D2D1_RECT_F b{};
    if (!img || FAILED(dc->GetImageLocalBounds(img.get(), &b)) || b.right <= b.left || b.bottom <= b.top) {
        Bitmap(dc, bmp, r, opacity);
        return;
    }
    D2D1_MATRIX_3X2_F base;
    dc->GetTransform(&base);
    float sx = (r.right - r.left) / (b.right - b.left), sy = (r.bottom - r.top) / (b.bottom - b.top);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(-b.left, -b.top) * D2D1::Matrix3x2F::Scale(sx, sy) *
                     D2D1::Matrix3x2F::Translation(r.left, r.top) * base);
    dc->DrawImage(img.get());
    dc->SetTransform(base);
}

} // namespace nb::draw
