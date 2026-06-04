// Sokol implementations — single-TU compile of sokol_app + sokol_gfx +
// sokol_glue + sokol_log. Vendored headers live in deps/.
//
// SOKOL_NO_ENTRY keeps the existing main() in src/main.cpp — we call
// sapp_run(sapp_desc) explicitly from there rather than letting sokol
// generate WinMain. Lets the migration be phased.
//
// Backend is picked per-platform: D3D11 on Windows, desktop GL (GLCORE,
// GLX/X11) everywhere else. main.cpp's present shaders branch on the same
// _WIN32 split (HLSL vs GLSL) so the two stay in lockstep.

#define SOKOL_IMPL
#if defined(_WIN32)
  #define SOKOL_D3D11
#else
  #define SOKOL_GLCORE
#endif
#define SOKOL_NO_ENTRY

#include "sokol_log.h"
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
