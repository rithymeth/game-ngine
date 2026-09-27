// The third-party decoders' implementations, compiled once (Phase 17 step 3).
// Ogg Vorbis: stb_vorbis (public domain). FLAC and WAV: dr_flac and dr_wav
// (public domain / MIT-0). They're used through audio/src/decoders.cpp.
#include <stb_vorbis.c>

#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
