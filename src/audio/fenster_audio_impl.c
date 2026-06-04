// Compiled as C (.c extension) so fenster_audio.h's implicit
// `lpData = int16_t*` conversion is legal. audio.cpp pulls in
// declarations only via FENSTER_HEADER + extern "C".

// C mode doesn't transitively pull stdint via windows.h like C++; without
// it the struct's int16_t buf[][] fails to parse.
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
// fenster_audio.h's Windows branch includes <mmsystem.h> before
// <windows.h>; mmsystem.h needs UINT/HANDLE/etc. which only get pulled
// in by windows.h. Pre-include here so the order is right.
#include <windows.h>
#endif

#define FENSTER_AUDIO_BUFSZ 882

// fenster_audio.h's Windows backend assigns `int16_t*` to LPSTR
// (the WAVEHDR.lpData field) — legal C since LPSTR is just a byte
// pointer and waveOut takes a raw byte stream, but MSVC flags it as
// C4133 by default. Silence locally rather than touching deps/.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4133)
#endif

#include "fenster_audio.h"

#ifdef _MSC_VER
#pragma warning(pop)
#endif
