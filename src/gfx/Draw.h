#pragma once
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <winrt/base.h>

#include <string_view>

namespace nb::draw {

enum class Align { Leading, Center, Trailing };

void Init(); // picks fonts available on this Windows version

// Symbol glyphs (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10).
namespace glyph {
constexpr wchar_t Volume0 = 0xE992, Volume1 = 0xE993, Volume2 = 0xE994, Volume3 = 0xE995, Mute = 0xE74F;
constexpr wchar_t Wifi1 = 0xE872, Wifi2 = 0xE873, Wifi3 = 0xE874, Wifi4 = 0xE701, Ethernet = 0xE839;
constexpr wchar_t NetOffline = 0xF384, WifiWarn = 0xEB5E, Globe = 0xE774;
constexpr wchar_t Battery0 = 0xE850, Battery10 = 0xE83F, BatteryCharging0 = 0xE85A, BatteryCharging10 = 0xE83E, BatteryUnknown = 0xE996;
constexpr wchar_t Plug = 0xE83E;
constexpr wchar_t Bell = 0xEA8F, BellOff = 0xE7ED;
constexpr wchar_t Music = 0xE8D6, Play = 0xE768, Pause = 0xE769, Previous = 0xE892, Next = 0xE893;
constexpr wchar_t Search = 0xE721, Settings = 0xE713, Add = 0xE710, Close = 0xE711, Back = 0xE72B, AllApps = 0xE71D;
constexpr wchar_t Star = 0xE734, StarFill = 0xE735, Folder = 0xE8B7, Document = 0xE8A5, Calculator = 0xE8EF;
constexpr wchar_t Copy = 0xE8C8, Rename = 0xE8AC, Delete = 0xE74D, Power = 0xE7E8, Restore = 0xE777;
constexpr wchar_t Mouse = 0xE962, Keyboard = 0xE765, Headset = 0xE95B, Headphones = 0xE7F6, Earbuds = 0xF4C0;
constexpr wchar_t Gamepad = 0xE7FC, Phone = 0xE8EA, Laptop = 0xE7F8, Bluetooth = 0xE702, Devices = 0xE772;
constexpr wchar_t ChevronRight = 0xE76C, ChevronLeft = 0xE76B, ChevronDown = 0xE70D, ChevronUp = 0xE70E, Check = 0xE73E;
constexpr wchar_t Info = 0xE946, Warning = 0xE7BA, Speaker = 0xE7F5, Up = 0xE74A, Down = 0xE74B;
} // namespace glyph

wchar_t BatteryGlyph(int percent, bool charging);
wchar_t VolumeGlyph(float level, bool muted);

ID2D1SolidColorBrush* Brush(ID2D1DeviceContext* dc, const D2D1_COLOR_F& c);

void Text(ID2D1DeviceContext* dc, std::wstring_view text, const D2D1_RECT_F& r, float size, const D2D1_COLOR_F& c,
          DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL, Align align = Align::Leading, bool tabularNumbers = false);
float TextWidth(std::wstring_view text, float size, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);
void Glyph(ID2D1DeviceContext* dc, wchar_t g, D2D1_POINT_2F center, float size, const D2D1_COLOR_F& c);

void Panel(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, float opacity = 1.0f, bool shadow = true);
void RoundRect(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius, const D2D1_COLOR_F& fill);
void FocusRing(ID2D1DeviceContext* dc, const D2D1_RECT_F& r, float radius);

// Notch outline: top edge flush with the screen, concave flares that blend into the edge and
// rounded bottom corners. x0/w describe the body; flares extend outside it.
winrt::com_ptr<ID2D1PathGeometry> NotchPath(float x0, float w, float h, float flare, float radius);

void Bitmap(ID2D1DeviceContext* dc, ID2D1Bitmap* bmp, const D2D1_RECT_F& r, float opacity = 1.0f);

} // namespace nb::draw
