// Compiled as C (note the .c extension — cl.exe picks the language by
// extension). C++ rejects fenster_audio.h's Windows backend because of
// the implicit `lpData = int16_t*` conversion at line 101, which is
// fine C but a hard error in C++. Compiling the implementation as C
// here lets the header-only file work without modifying deps/.
//
// The matching audio.cpp wraps its include in `extern "C"` and defines
// FENSTER_HEADER so that side gets only the declarations — the symbols
// resolve to the definitions in this TU.

// <stdint.h> is needed for int16_t in the struct definition. C++ mode
// pulls it in transitively via <windows.h>, but C mode (this TU) does
// not, so without this include the struct's int16_t buf[][] field
// fails to parse and every subsequent reference to fenster_audio
// reads as "undefined struct/union".
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
