// Dreamcast-native audio streaming backend for Smacker (.smk) cutscene audio.
//
// miniaudio's ma_engine/ma_sound/ma_paged_audio_buffer stack is built around
// finite, seekable sources (files) or fixed-size buffers - not an
// indefinitely-growing live PCM stream fed in small increments forever.
// Every bug chased while that stack handled this (a cursor/trimming
// mismatch, ma_sound_start()'s built-in seek-to-0-on-restart silently
// undoing our own resume position, a deferred seekTarget race, a
// skip-vs-restart deadlock) came from fighting assumptions baked into that
// layer, not from logic errors of our own. A Smacker stream never seeks,
// never needs spatialization or engine mixing, and is always exactly one
// sequential producer (decode) and one consumer (playback) - so this talks
// to KallistiOS's own sound streaming API (dc/sound/stream.h) directly with
// a plain ring buffer instead.
//
// Design: AudioBackend_StreamWrite (the decode/producer side, called once
// per video frame from the main thread) copies decoded PCM into a ring
// buffer and calls snd_stream_poll(), which synchronously invokes
// DCStream_Callback (the consumer side) if the AICA's double-buffer needs
// refilling. Both sides only ever run on this one thread, in this one
// function call - there is no concurrent access to the ring buffer, no
// seeking, no atEnd state machine, so none of the desync bugs above are
// possible by construction. Overflow (push outpacing drain) just drops the
// incoming chunk instead of growing further; underflow (drain outpacing
// push) just returns NULL from the callback, which KOS's stream driver
// fills with silence. Both recover on their own as soon as the mismatch
// reverses - no caps, no hysteresis, no restart logic needed.

#include "harness/audio.h"
#include "harness/trace.h"

#include <kos.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// How far ahead of real-time playback we're willing to buffer before
// dropping new pushes. Generous, but still bounds worst-case memory growth
// the same way the original "memory runs full" bug report needed - just
// with a plain ring buffer instead of a page list, so there's no separate
// state machine that can desync from it.
#define DC_AUDIO_RING_SECONDS 2

// Upper bound on a single snd_stream_poll() request. Real requests are
// governed by the stream's buffer size (SND_STREAM_BUFFER_MAX, 64KB per
// channel below - see DCStream_Callback), so this is a large safety margin,
// not a tuned value.
#define DC_AUDIO_SCRATCH_BYTES (32 * 1024)

// Per-channel buffer KOS keeps in SPU RAM for the stream. snd_stream_poll()
// asks our callback to refill (this size)/2 PER-CHANNEL bytes - i.e. exactly
// `this size` total interleaved bytes for a stereo stream - whenever
// per-channel playback crosses into the other half. Two constraints in
// tension:
//
//  - That half-buffer must drain slower than AudioBackend_StreamWrite's push
//    interval (once per decoded video frame, ~MSPerFrame apart, commonly
//    ~66ms), or the boundary check keeps landing in the trough between two
//    pushes and failing even though the ring isn't actually empty overall.
//    Per-channel drain time is (this size / 2) / sample_rate (NOT divided by
//    channel count - each channel's hardware clock advances independently
//    at the stream's sample rate regardless of channel count).
//  - The single combined request size (= this size, for stereo) must stay
//    well below what the ring typically has resident, or it reliably fails
//    even with a healthy ring. Diagnostics confirmed this dominates: going
//    from 4KB to 16KB only lengthened the drain margin (already comfortable
//    at 4KB - 93ms versus a ~66ms push gap) while roughly tripling the
//    underrun rate (24-32% at 4KB vs 103-110% at 16KB), because the ring
//    (which typically holds just 3-24KB) could no longer reliably supply a
//    16KB ask in one shot.
#define DC_AUDIO_STREAM_BUFFER_BYTES (4 * 1024)

// TEMPORARY diagnostic: force Smacker audio down to mono (one AICA channel,
// no pan split at all) to find out whether the reported left/right panning
// artifact is specific to the stereo two-channel pairing/pan logic, or comes
// from something else (e.g. timing/underruns) that would still show up in
// mono. Revert by setting this back to 0.
#define DC_AUDIO_FORCE_MONO_DIAGNOSTIC 0

typedef struct tDCStream {
    snd_stream_hnd_t hnd;
    unsigned int bit_depth;
    unsigned int input_channels; // channels in the data passed to StreamWrite, before any downmix
    unsigned int frame_size_in_bytes; // bytes per sample-frame, all channels, in the RING (post-downmix)
    unsigned char* ring;
    unsigned int ring_cap;
    unsigned int ring_read;
    unsigned int ring_used;
    unsigned char scratch[DC_AUDIO_SCRATCH_BYTES];
#if DC_AUDIO_FORCE_MONO_DIAGNOSTIC
    unsigned char downmix_scratch[DC_AUDIO_SCRATCH_BYTES];
#endif
    // Temporary diagnostics for tuning the buffer size/poll frequency
    // against real hardware behaviour.
    unsigned long diag_write_calls;
    unsigned long diag_poll_calls;
    unsigned long diag_underruns;  // DCStream_Callback had nothing to give
    unsigned long diag_drops;      // StreamWrite dropped a push (overflow)
    unsigned long diag_misaligned; // StreamWrite got a push not a multiple of frame_size_in_bytes
} tDCStream;

static int g_dc_stream_subsystem_ready = 0;
// At most one Smacker video plays at a time - tracked here so
// DCSmackerStream_Poll() (called from SmackWait, see below) has something
// to poll without smackw32.c needing to know our internal stream type.
static tDCStream* g_active_stream = NULL;

// Called synchronously from inside snd_stream_poll() (see AudioBackend_
// StreamWrite/DCSmackerStream_Poll below) whenever the AICA's double-buffer
// needs refilling. smp_req/smp_recv are named for samples but are actually
// byte counts here (see snd_stream_fill() in KOS: needed_bytes is passed
// straight through as smp_req).
static void* DCStream_Callback(snd_stream_hnd_t hnd, int smp_req, int* smp_recv) {
    tDCStream* s = (tDCStream*)snd_stream_get_userdata(hnd);
    unsigned int want = (unsigned int)smp_req;

    // Must return either exactly the full amount requested or nothing at
    // all (never a short, oddly-sized chunk): KOS computed smp_req already
    // aligned for the G2 DMA transfer it's about to do with whatever we
    // return, and a partial amount - even frame-aligned - isn't guaranteed
    // to satisfy that. Returning a partial chunk here once produced
    // "g2_dma: Unaligned g2bus DMA" followed by a hard crash. Falling back
    // to real silence on underrun is the safe choice; the buffer size and
    // polling frequency (see DC_AUDIO_STREAM_BUFFER_BYTES and
    // DCSmackerStream_Poll) are what actually need to keep this satisfied,
    // not a relaxed return-whatever-we-have policy here.
    if (s == NULL || want == 0 || want > sizeof(s->scratch) || s->ring_used < want) {
        if (s != NULL) {
            s->diag_underruns++;
        }
        *smp_recv = 0;
        return NULL;
    }

    {
        unsigned int first = s->ring_cap - s->ring_read;
        if (first > want) {
            first = want;
        }
        memcpy(s->scratch, s->ring + s->ring_read, first);
        if (want > first) {
            memcpy(s->scratch + first, s->ring, want - first);
        }
        s->ring_read = (s->ring_read + want) % s->ring_cap;
        s->ring_used -= want;
    }

    *smp_recv = (int)want;
    return s->scratch;
}

// Called from SmackWait()'s busy-wait loop (see smackw32.c), much more
// often than once per decoded video frame, so the AICA's buffer gets
// refilled promptly instead of only when the next frame happens to finish
// decoding. Matches the pattern of a different Dreamcast dethrace fork
// (GPF/dethrace) servicing its own audio backend from inside SmackWait.
void DCSmackerStream_Poll(void) {
    if (g_active_stream != NULL) {
        g_active_stream->diag_poll_calls++;
        snd_stream_poll(g_active_stream->hnd);
    }
}

tAudioBackend_stream* AudioBackend_StreamOpen(int bit_depth, int channels, unsigned int sample_rate) {
    tDCStream* s;
    unsigned int bytes_per_sample = (bit_depth == 8) ? 1u : 2u; // Smacker PCM is always 8 or 16 bit

    if (!g_dc_stream_subsystem_ready) {
        // snd_init() (called internally here) is idempotent - it no-ops if
        // miniaudio's own DC backend has already brought up the AICA for
        // SFX/CDA, so this is safe to call regardless of init order.
        if (snd_stream_init() < 0) {
            LOG_WARN("snd_stream_init failed");
            return NULL;
        }
        // miniaudio's DC backend hardcodes AICA channels 0 and 1 for its
        // own device (ma_device_config_dreamcast's voiceChannel defaults to
        // 0, used directly for stereo output) without ever registering them
        // with KOS's own channel allocator (snd_sfx_chn_alloc(), used below
        // by snd_stream_alloc()). That allocator has no idea those channels
        // are taken, so without this it hands the exact same channels 0/1
        // to our stream too - both systems then fight over the same
        // physical AICA channels, which is consistent with everything
        // observed: garbled/loud Smacker audio (two signals fighting for
        // the same channel) and silent in-game SFX/CDA (miniaudio's channel
        // state gets stomped by our stream's). Reserve and permanently
        // leak channels 0 and 1 here so the allocator's first real pick for
        // our stream lands on 2/3 instead, leaving miniaudio's hardcoded
        // channels alone.
        {
            int reserved0 = snd_sfx_chn_alloc();
            int reserved1 = snd_sfx_chn_alloc();
            printf("[dc-audio] reserved channels %d,%d away from miniaudio\n", reserved0, reserved1);
        }
        g_dc_stream_subsystem_ready = 1;
    }

    s = malloc(sizeof(tDCStream));
    if (s == NULL) {
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->bit_depth = (unsigned int)bit_depth;
    s->input_channels = (unsigned int)channels;
#if DC_AUDIO_FORCE_MONO_DIAGNOSTIC
    // Ring/stream are sized for mono output regardless of the real input
    // channel count - AudioBackend_StreamWrite downmixes before pushing.
    s->frame_size_in_bytes = bytes_per_sample;
#else
    s->frame_size_in_bytes = bytes_per_sample * (unsigned int)channels;
#endif
    s->ring_cap = sample_rate * s->frame_size_in_bytes * DC_AUDIO_RING_SECONDS;
    s->ring = malloc(s->ring_cap);
    if (s->ring == NULL) {
        free(s);
        return NULL;
    }

    s->hnd = snd_stream_alloc(NULL, DC_AUDIO_STREAM_BUFFER_BYTES);
    if (s->hnd == SND_STREAM_INVALID) {
        LOG_WARN("snd_stream_alloc failed");
        free(s->ring);
        free(s);
        return NULL;
    }
    snd_stream_set_userdata(s->hnd, s);
    snd_stream_set_callback(s->hnd, DCStream_Callback);

#if DC_AUDIO_FORCE_MONO_DIAGNOSTIC
    if (bit_depth == 8) {
        snd_stream_start_pcm8(s->hnd, sample_rate, 0);
    } else {
        snd_stream_start(s->hnd, sample_rate, 0);
    }
#else
    if (bit_depth == 8) {
        snd_stream_start_pcm8(s->hnd, sample_rate, channels == 2);
    } else {
        snd_stream_start(s->hnd, sample_rate, channels == 2);
    }
#endif

    printf("[dc-audio] smacker stream hnd=%d bitdepth=%d channels=%d rate=%u ring_cap=%u mono_diag=%d\n",
        s->hnd, bit_depth, channels, sample_rate, s->ring_cap, DC_AUDIO_FORCE_MONO_DIAGNOSTIC);

    g_active_stream = s;
    return (tAudioBackend_stream*)s;
}

tAudioBackend_error_code AudioBackend_StreamWrite(void* stream_handle, const unsigned char* data, unsigned long size) {
    tDCStream* s = (tDCStream*)stream_handle;
    unsigned int free_space;

    s->diag_write_calls++;

#if DC_AUDIO_FORCE_MONO_DIAGNOSTIC
    // TEMPORARY: downmix interleaved input to mono before anything else, so
    // the rest of this function (and the ring/AICA channel) never see
    // stereo data at all - see DC_AUDIO_FORCE_MONO_DIAGNOSTIC's comment.
    if (s->input_channels == 2) {
        unsigned long mono_size = size / 2; // 2 input bytes (one L+R 8-bit frame) -> 1 output byte
        unsigned long i;
        if (mono_size > sizeof(s->downmix_scratch)) {
            mono_size = sizeof(s->downmix_scratch);
        }
        for (i = 0; i < mono_size; i++) {
            // Still raw unsigned 8-bit here (sign conversion happens below),
            // so a plain unsigned average is correct.
            unsigned int l = data[i * 2 + 0];
            unsigned int r = data[i * 2 + 1];
            s->downmix_scratch[i] = (unsigned char)((l + r) / 2);
        }
        data = s->downmix_scratch;
        size = mono_size;
    }
#endif

    // smk_get_audio_size() is expected to always return a whole number of
    // sample-frames, but if it ever doesn't, pushing the odd trailing byte
    // would permanently shift every later read's left/right alignment by one
    // byte - heard as the stereo image abruptly swapping sides ("left box,
    // right box") rather than smoothly panning, since every subsequent
    // sample would land in the other channel's slot until another odd-sized
    // push happened to shift it back. Defend against that unconditionally by
    // only ever pushing whole frames; diag_misaligned tracks whether this
    // ever actually fires on real files.
    if ((size % s->frame_size_in_bytes) != 0) {
        s->diag_misaligned++;
        size -= (size % s->frame_size_in_bytes);
    }

    // Give the AICA's double-buffer a chance to refill before we look at
    // how much room is left - this is the only place the consumer side
    // (DCStream_Callback) ever runs, synchronously, on this same thread.
    snd_stream_poll(s->hnd);

    free_space = s->ring_cap - s->ring_used;
    if (size > free_space) {
        // Buffered as far ahead as we're willing to go - drop this push
        // rather than grow further. Self-correcting: real playback keeps
        // draining the ring regardless, so free_space recovers on its own
        // without any restart/seek logic needed.
        s->diag_drops++;
    } else {
        unsigned int write_pos = (s->ring_read + s->ring_used) % s->ring_cap;
        unsigned int first = s->ring_cap - write_pos;
        unsigned int i;
        if (first > size) {
            first = (unsigned int)size;
        }
        // Smacker's 8-bit PCM is unsigned (0..255, centered on 128, the same
        // convention as 8-bit WAV), but the AICA's PCM8 channel format is
        // signed (-128..127). Feeding unsigned data straight to a
        // signed-expecting channel flips the effective sign of every sample
        // whose high bit happens to be set, which sounded exactly like what
        // was reported: harsh/clipped, too loud, wildly swinging amplitude.
        // 16-bit Smacker audio is already signed (the normal PCM16
        // convention), so this conversion is only needed for 8-bit.
        if (s->bit_depth == 8) {
            for (i = 0; i < first; i++) {
                s->ring[write_pos + i] = (unsigned char)(data[i] ^ 0x80u);
            }
            for (i = 0; i < size - first; i++) {
                s->ring[i] = (unsigned char)(data[first + i] ^ 0x80u);
            }
        } else {
            memcpy(s->ring + write_pos, data, first);
            if (size > first) {
                memcpy(s->ring, data + first, size - first);
            }
        }
        s->ring_used += (unsigned int)size;
    }

    if ((s->diag_write_calls % 30) == 0) {
        printf("[dc-audio] writes=%lu polls=%lu underruns=%lu drops=%lu misaligned=%lu ring_used=%u/%u\n",
            s->diag_write_calls, s->diag_poll_calls, s->diag_underruns, s->diag_drops,
            s->diag_misaligned, s->ring_used, s->ring_cap);
    }

    return eAB_success;
}

tAudioBackend_error_code AudioBackend_StreamClose(tAudioBackend_stream* stream_handle) {
    tDCStream* s = (tDCStream*)stream_handle;
    if (s == NULL) {
        return eAB_success;
    }
    if (g_active_stream == s) {
        g_active_stream = NULL;
    }
    snd_stream_stop(s->hnd);
    snd_stream_destroy(s->hnd);
    free(s->ring);
    free(s);
    return eAB_success;
}
