// stb_vorbis implementation compiled as its own translation unit.
//
// It used to be #included directly into miniaudio.c, but on the Dreamcast that
// pulls in KallistiOS headers (via miniaudio's AICA backend) whose native
// int8/int32/uint32 typedefs clash with stb_vorbis's own. Building it
// standalone keeps stb_vorbis away from miniaudio. miniaudio.c still includes
// the stb_vorbis header (declarations only) so its Vorbis decoding backend
// links against the symbols defined here.

#include "stb/stb_vorbis.c"
