#include "smackw32/smackw32.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __DREAMCAST__
#include <malloc.h>
#endif

#include "harness/hooks.h"
#include "harness/os.h"
#include "harness/trace.h"

// lib/libsmacker
#include "smacker.h"

static unsigned int smack_last_frame_time = 0;

#ifdef __DREAMCAST__
// Defined in harness/audio/dc_smacker_stream.c. Not part of the
// cross-platform harness/audio.h interface since it's specific to how the
// Dreamcast backend needs servicing - see the comment on that function.
extern void DCSmackerStream_Poll(void);
#endif

static void copy_palette(Smack* smack) {
    const unsigned char* pal = smk_get_palette(smack->smk_handle);
    memcpy(smack->Palette, pal, 256 * 3);
}

Smack* SmackOpen(const char* name, unsigned int flags, unsigned int extrabuf) {
    unsigned char track_mask_smk;
    unsigned char channels_smk[7];
    unsigned char bitdepth_smk[7];
    unsigned long sample_rate_smk[7];
    double microsecs_per_frame;
    Smack* smack;
    double fps;
    FILE* f = NULL;
    smk smk_handle;

    f = OS_fopen(name, "rb");
    if (f == NULL) {
        return NULL;
    }
    // SMK_MODE_MEMORY preloads every frame's compressed chunk data into RAM
    // up front - for a full FMV that's several MB, which exhausts the DC's
    // 16MB budget mid-playback (stutter, then a crash/restart). SMK_MODE_DISK
    // streams each frame's chunk from disk on demand instead.
#ifdef __DREAMCAST__
    smk_handle = smk_open_filepointer(f, SMK_MODE_DISK);
#else
    smk_handle = smk_open_filepointer(f, SMK_MODE_MEMORY);
#endif
    if (smk_handle == NULL) {
        fclose(f);
        return NULL;
    }

    smack = malloc(sizeof(Smack));

    // libsmacker doesn't tell us whether the palette is new on each frame or not, so just assume it always is new
    smack->NewPalette = 1;

    // smk_handle is added to hold a pointer to the underlying libsmacker instance
    smack->smk_handle = smk_handle;

    smack->f = f;

    smk_info_all(smk_handle, NULL, &smack->Frames, &microsecs_per_frame);
    fps = 1000000.0 / microsecs_per_frame;
    smack->MSPerFrame = (unsigned long)((1 / fps) * 1000);
    smk_info_video(smk_handle, &smack->Width, &smack->Height, NULL);
    smk_enable_video(smk_handle, 1);

    // get info about the audio tracks in this video
    smk_info_audio(smk_handle, &track_mask_smk, channels_smk, bitdepth_smk, sample_rate_smk);

    smack->audio_stream = NULL;
    if ((track_mask_smk & SMK_AUDIO_TRACK_0)) {
        smack->audio_stream = AudioBackend_StreamOpen(bitdepth_smk[0], channels_smk[0], sample_rate_smk[0]);
        if (smack->audio_stream != NULL) {
            // tell libsmacker we can process audio now
            smk_enable_audio(smk_handle, 0, 1);
        }
    }

    // load the first frame and return a handle to the Smack file
    if (smk_first(smk_handle) == SMK_ERROR) {
        smk_close(smk_handle);
        free(smack);
        return NULL;
    }
    copy_palette(smack);
    return smack;
}

int SmackSoundUseDirectSound(void* dd) {
    return 0;
}

void SmackToBuffer(Smack* smack, unsigned int left, unsigned int top, unsigned int pitch, unsigned int destheight, void* buf, unsigned int flags) {
    unsigned long i; // Pierre-Marie Baty -- fixed type
    char* char_buf = buf;
    const unsigned char* frame;

    // minimal implementation
    assert(left == 0);
    assert(top == 0);
    assert(flags == 0);

    frame = smk_get_video(smack->smk_handle);
    for (i = 0; i < smack->Height; i++) {
        memcpy(&char_buf[(i * pitch)], &frame[i * smack->Width], smack->Width);
    }
}

int SmackDoFrame(Smack* smack) {
    const unsigned char* audio_data;
    unsigned long audio_data_size;

    // process audio if we have some
    if (smack->audio_stream != NULL) {
        audio_data = smk_get_audio(smack->smk_handle, 0);
        audio_data_size = smk_get_audio_size(smack->smk_handle, 0);
        if ((audio_data == NULL) || (audio_data_size == 0)) {
            return 0;
        }

        AudioBackend_StreamWrite(smack->audio_stream, audio_data, audio_data_size);
    }

    return 0;
}

void SmackNextFrame(Smack* smack) {
    // Temporary diagnostic: measure actual decode time against the frame's
    // real-time budget (MSPerFrame), to check whether decode is the
    // bottleneck behind the cutscene audio backlog/crash investigation.
    // Prints only on a new worst-case duration, so this stays quiet unless
    // decode is genuinely struggling.
    static unsigned int g_diag_max_decode_ms = 0;
    static int g_diag_frame_count = 0;
    unsigned int _diag_t0 = gHarness_platform.GetTicks();
    g_diag_frame_count++;

    smk_next(smack->smk_handle);
    copy_palette(smack);

    // (Tried adding an extra DCSmackerStream_Poll() call here, on the theory
    // that smk_next()'s tens-of-ms decode time was an unserviced gap. Made
    // underruns measurably worse instead: KallistiOS's snd_stream_poll(),
    // once playback is in the normal "ahead" state, has no half-buffer gate
    // at all and attempts a real fill - including a real silence
    // memset+DMA on failure - on every single call, so an extra poll when
    // the ring is thin just means extra wasted fill attempts, not extra
    // chances to catch up. Reverted; SmackWait's existing per-spin polling
    // is the only servicing point.)

    {
        unsigned int _diag_dt = gHarness_platform.GetTicks() - _diag_t0;
        if (_diag_dt > g_diag_max_decode_ms) {
            g_diag_max_decode_ms = _diag_dt;
            printf("[smk-diag] decode new max %ums (budget %lums)\n", g_diag_max_decode_ms, smack->MSPerFrame);
        }
    }

    // Decode time has been observed to climb steadily over a single video
    // (a few ms up to 40-70ms) instead of staying flat, which shouldn't
    // happen for decoding the same kind of frame repeatedly. Track heap
    // health alongside it to see whether this is fragmentation/a leak
    // (used space growing, free space shrinking or getting choppier) rather
    // than something in the decode algorithm itself.
#ifdef __DREAMCAST__
    if ((g_diag_frame_count % 30) == 0) {
        struct mallinfo mi = mallinfo();
        printf("[heap-diag] frame=%d used=%d free=%d free_chunks=%d\n",
            g_diag_frame_count, mi.uordblks, mi.fordblks, mi.ordblks);
    }
#endif
}

int SmackWait(Smack* smack) {
    unsigned int now = gHarness_platform.GetTicks();
#ifdef __DREAMCAST__
    // The AICA buffer needs refilling far more often than once per decoded
    // video frame (see DCSmackerStream_Poll's comment) - service it on
    // every spin of this wait loop too, not just when a new frame finishes
    // decoding, matching how a different Dreamcast dethrace fork
    // (GPF/dethrace) services its own audio backend from inside SmackWait.
    DCSmackerStream_Poll();
#endif
    if (now < smack_last_frame_time + smack->MSPerFrame) {
        gHarness_platform.Sleep(1);
        return 1;
    }
    smack_last_frame_time = now;
    return 0;
}

#ifdef __DREAMCAST__
// Used by cutscene.c's decode-ahead logic (see the comment there for why):
// reports whether there's still time left in the current frame's display
// slot, using the exact same deadline SmackWait() itself waits against, so
// idle CPU time that would otherwise just be spent sleeping/polling can
// instead be used to decode (and push audio for) further frames.
int DCSmackHasIdleBudget(Smack* smack) {
    return gHarness_platform.GetTicks() < smack_last_frame_time + smack->MSPerFrame;
}
#endif

void SmackClose(Smack* smack) {
    if (smack->audio_stream != NULL) {
        AudioBackend_StreamClose(smack->audio_stream);
    }

    smk_close(smack->smk_handle);
    // libsmacker closes file, no need to do `fclose(smack->f)`
    free(smack);
}
