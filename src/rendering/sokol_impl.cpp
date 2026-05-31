// Sokol implementations — single-TU compile of sokol_app + sokol_gfx +
// sokol_glue + sokol_log. Vendored headers live in deps/.
//
// SOKOL_NO_ENTRY keeps the existing main() in src/main.cpp — we call
// sapp_run(sapp_desc) explicitly from there rather than letting sokol
// generate WinMain. Lets the migration be phased.
//
// SOKOL_D3D11 pins the backend on Windows; no GL fallback compiled in.

#define SOKOL_IMPL
#define SOKOL_D3D11
#define SOKOL_NO_ENTRY

#include "sokol_log.h"
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
