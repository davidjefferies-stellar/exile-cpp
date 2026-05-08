// Single TU that compiles fenster.h's window / input implementation.
// Mirrors fenster_audio_impl.c on the audio side: every other TU that
// includes fenster.h does so via pixel_renderer.h, which sets
// FENSTER_HEADER first to suppress the body. The symbols those TUs
// reference (fenster_open / _loop / _close / _sleep / _time) resolve
// to the definitions emitted here.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef UNICODE
#undef UNICODE
#endif
#ifdef _UNICODE
#undef _UNICODE
#endif

#include "fenster.h"
