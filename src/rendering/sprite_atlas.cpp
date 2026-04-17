#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif

#include "rendering/sprite_atlas.h"
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "gdiplus.lib")

static uint32_t g_atlas[ATLAS_W * ATLAS_H];
static bool g_loaded = false;

const uint32_t* atlas_pixels() { return g_atlas; }

bool atlas_load(const char* png_path) {
    if (g_loaded) return true;

    Gdiplus::GdiplusStartupInput startup{};
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &startup, nullptr) != Gdiplus::Ok) {
        return false;
    }

    wchar_t wpath[MAX_PATH];
    size_t converted = 0;
    mbstowcs_s(&converted, wpath, MAX_PATH, png_path, _TRUNCATE);

    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromFile(wpath, FALSE);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        Gdiplus::GdiplusShutdown(token);
        return false;
    }

    Gdiplus::Rect rect(0, 0, ATLAS_W, ATLAS_H);
    Gdiplus::BitmapData bd{};
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead,
                      PixelFormat32bppARGB, &bd) != Gdiplus::Ok) {
        delete bmp;
        Gdiplus::GdiplusShutdown(token);
        return false;
    }

    for (int y = 0; y < ATLAS_H; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(bd.Scan0) + y * bd.Stride);
        std::memcpy(&g_atlas[y * ATLAS_W], row, ATLAS_W * sizeof(uint32_t));
    }

    bmp->UnlockBits(&bd);
    delete bmp;
    g_loaded = true;
    return true;
}
