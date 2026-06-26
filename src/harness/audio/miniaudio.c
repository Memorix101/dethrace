// Disable miniaudio's 'null' device fallback. A proper device must be found to enable playback
#define MA_NO_NULL

#include "harness/audio.h"
#include "harness/config.h"
#include "harness/hooks.h"
#include "harness/os.h"
#include "harness/trace.h"

// Must come before miniaudio.h
#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"

// The stb_vorbis implementation is compiled in its own translation unit
// (stb_vorbis_impl.c) so it does not collide with platform headers pulled in by
// miniaudio. Only the header (declarations) is needed here.

#include <assert.h>
#include <stdio.h>
#include <string.h>

// duplicates DETHRACE/constants.h but is a necessary evil(?)
static int kMem_S3_DOS_SOS_channel = 234;

typedef struct tMiniaudio_sample {
    ma_audio_buffer buffer_ref;
    ma_sound sound;
    int init_volume;
    int init_pan;
    int init_new_rate;
    int initialized;
} tMiniaudio_sample;

// On Dreamcast, the Smacker streaming path (AudioBackend_StreamOpen/Write/
// Close below) is implemented natively against KOS in dc_smacker_stream.c
// instead - see the comment at the top of that file for why. Everything
// else in this file (SFX, CDA) is untouched and still goes through
// miniaudio on every platform, including Dreamcast.
#ifndef __DREAMCAST__
typedef struct tMiniaudio_stream {
    int frame_size_in_bytes;
    int sample_rate;
    int needs_converting;
    ma_paged_audio_buffer_data paged_audio_buffer_data;
    ma_paged_audio_buffer paged_audio_buffer;
    ma_data_converter data_converter;
    ma_sound sound;
    // ma_paged_audio_buffer_get_length_in_pcm_frames() walks the entire page
    // linked list to sum it up - fine for a one-off query, but StreamWrite
    // called it on every single write, making each write linearly more
    // expensive as a video plays since pages are never freed until the
    // stream closes. Track the same value incrementally instead.
    ma_uint64 total_frames_pushed;
    // Pages are appended every write but otherwise never freed until
    // ma_sound_uninit() at stream close - on a long-enough video that's
    // unbounded growth in decoded (uncompressed) PCM, which is exactly what
    // exhausts the Dreamcast's 16MB on real hardware after enough playback
    // (an emulator with looser memory limits may never hit it). This tracks
    // how many leading frames have already been freed, so the trim pass in
    // AudioBackend_StreamWrite knows the true absolute start position of
    // whatever page is now at the front of the list.
    ma_uint64 total_frames_freed;
    // Hysteresis state for the backlog cap in AudioBackend_StreamWrite: once
    // the backlog crosses the high watermark this latches true and stays
    // true (continuing to skip new pushes) until it drains back down to the
    // low watermark, rather than re-checking a single threshold every call.
    // Needed because push genuinely outpaces drain by a small, sustained
    // amount on real hardware (the AICA's actual playback clock appears to
    // run slightly differently from the nominal rate we compute against) -
    // a single-threshold check just dips under the cap for one call, lets a
    // push through immediately, and the backlog resumes its slow climb -
    // confirmed via diagnostics showing resident growing far past the cap
    // over time despite the skip logic technically being in place.
    int throttling;
} tMiniaudio_stream;
#endif // !__DREAMCAST__

ma_engine engine;
ma_sound cda_sound;
int cda_sound_initialized;
int ma_engine_initialized;

tAudioBackend_error_code AudioBackend_Init(void) {
    ma_result result;
    ma_engine_config config;

    config = ma_engine_config_init();
    result = ma_engine_init(&config, &engine);
    if (result != MA_SUCCESS) {
        printf("Failed to initialize audio engine.");
        return eAB_error;
    }
    LOG_INFO("Audio playback device initialized");
    ma_engine_set_volume(&engine, harness_game_config.volume_multiplier);
    ma_engine_initialized = 1;

    return eAB_success;
}

tAudioBackend_error_code AudioBackend_InitCDA(void) {
    // check if music files are present or not
    if (access("MUSIC/Track02.ogg", F_OK) == -1) {
        printf("Failed to find music files, disabling CDA support.");
        return eAB_error;
    }
    return eAB_success;
}

void AudioBackend_UnInit(void) {
    ma_engine_uninit(&engine);
    ma_engine_initialized = 0;
}

void AudioBackend_UnInitCDA(void) {
    printf("Uninitializing CDA support.");
}

tAudioBackend_error_code AudioBackend_StopCDA(void) {
    if (!cda_sound_initialized) {
        return eAB_success;
    }
    if (ma_sound_is_playing(&cda_sound)) {
        ma_sound_stop(&cda_sound);
    }
    ma_sound_uninit(&cda_sound);
    cda_sound_initialized = 0;
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_PlayCDA(int track) {
    char path[256];
    ma_result result;

    sprintf(path, "MUSIC/Track0%d.ogg", track);

    if (access(path, F_OK) == -1) {
        return eAB_error;
    }

    // ensure we are not still playing a track
    AudioBackend_StopCDA();

    result = ma_sound_init_from_file(&engine, path, 0, NULL, NULL, &cda_sound);
    if (result != MA_SUCCESS) {
        return eAB_error;
    }
    cda_sound_initialized = 1;
    result = ma_sound_start(&cda_sound);
    if (result != MA_SUCCESS) {
        return eAB_error;
    }
    return eAB_success;
}

int AudioBackend_CDAIsPlaying(void) {
    if (!cda_sound_initialized) {
        return 0;
    }
    return ma_sound_is_playing(&cda_sound);
}

tAudioBackend_error_code AudioBackend_SetCDAVolume(int volume) {
    if (!cda_sound_initialized) {
        return eAB_error;
    }
    ma_sound_set_volume(&cda_sound, volume / 255.0f);
    return eAB_success;
}

void* AudioBackend_AllocateSampleTypeStruct(void) {
    tMiniaudio_sample* sample_struct;
    sample_struct = BrMemAllocate(sizeof(tMiniaudio_sample), kMem_S3_DOS_SOS_channel);
    if (sample_struct == NULL) {
        return 0;
    }
    memset(sample_struct, 0, sizeof(tMiniaudio_sample));
    return sample_struct;
}

tAudioBackend_error_code AudioBackend_PlaySample(void* type_struct_sample, int channels, void* data, int size, int rate, int loop) {
    tMiniaudio_sample* miniaudio;
    ma_result result;
    ma_int32 flags;

    miniaudio = (tMiniaudio_sample*)type_struct_sample;
    assert(miniaudio != NULL);

    ma_audio_buffer_config buffer_config = ma_audio_buffer_config_init(ma_format_u8, channels, rate, (ma_uint64)(size / channels), data, NULL);
    // init (not init_and_copy_data) references the caller's buffer, matching the
    // previous ma_audio_buffer_ref behaviour. The game keeps the sample alive.
    result = ma_audio_buffer_init(&buffer_config, &miniaudio->buffer_ref);
    if (result != MA_SUCCESS) {
        return eAB_error;
    }

    flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
    result = ma_sound_init_from_data_source(&engine, &miniaudio->buffer_ref, flags, NULL, NULL, &miniaudio->sound);
    if (result != MA_SUCCESS) {
        return eAB_error;
    }
    miniaudio->initialized = 1;

    if (miniaudio->init_volume > 0) {
        AudioBackend_SetVolume(type_struct_sample, miniaudio->init_volume);
        AudioBackend_SetPan(type_struct_sample, miniaudio->init_pan);
        AudioBackend_SetFrequency(type_struct_sample, rate, miniaudio->init_new_rate);
    }

    ma_sound_set_looping(&miniaudio->sound, loop);
    ma_sound_start(&miniaudio->sound);
    return eAB_success;
}

int AudioBackend_SoundIsPlaying(void* type_struct_sample) {
    tMiniaudio_sample* miniaudio;

    miniaudio = (tMiniaudio_sample*)type_struct_sample;
    assert(miniaudio != NULL);

    if (!miniaudio->initialized) {
        return 0;
    }

    if (ma_sound_is_playing(&miniaudio->sound)) {
        return 1;
    }
    return 0;
}

tAudioBackend_error_code AudioBackend_SetVolume(void* type_struct_sample, int volume) {
    tMiniaudio_sample* miniaudio;
    float linear_volume;

    miniaudio = (tMiniaudio_sample*)type_struct_sample;
    assert(miniaudio != NULL);

    if (!miniaudio->initialized) {
        miniaudio->init_volume = volume;
        return eAB_success;
    }

    linear_volume = volume / 510.0f;
    ma_sound_set_volume(&miniaudio->sound, linear_volume);
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_SetPan(void* type_struct_sample, int pan) {
    tMiniaudio_sample* miniaudio;

    miniaudio = (tMiniaudio_sample*)type_struct_sample;
    assert(miniaudio != NULL);

    if (!miniaudio->initialized) {
        miniaudio->init_pan = pan;
        return eAB_success;
    }

    // convert from directsound -10000 - 10000 pan scale
    ma_sound_set_pan(&miniaudio->sound, pan / 10000.0f);
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_SetFrequency(void* type_struct_sample, int original_rate, int new_rate) {
    tMiniaudio_sample* miniaudio;

    miniaudio = (tMiniaudio_sample*)type_struct_sample;
    assert(miniaudio != NULL);

    if (!miniaudio->initialized) {
        miniaudio->init_new_rate = new_rate;
        return eAB_success;
    }

    // convert from directsound frequency to linear pitch scale
    ma_sound_set_pitch(&miniaudio->sound, (new_rate / (float)original_rate));
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_SetVolumeSeparate(void* type_struct_sample, int left_volume, int right_volume) {
    return eAB_error;
}

tAudioBackend_error_code AudioBackend_StopSample(void* type_struct_sample) {
    tMiniaudio_sample* miniaudio;

    miniaudio = (tMiniaudio_sample*)type_struct_sample;
    assert(miniaudio != NULL);

    if (miniaudio->initialized) {
        ma_sound_stop(&miniaudio->sound);
        ma_sound_uninit(&miniaudio->sound);
        ma_audio_buffer_uninit(&miniaudio->buffer_ref);
        miniaudio->initialized = 0;
    }
    return eAB_success;
}

// On Dreamcast, the Smacker streaming path is implemented natively against
// KOS in dc_smacker_stream.c instead - see the comment at the top of that
// file for why. Everything above and below this block (SFX, CDA) is
// untouched and still goes through miniaudio on every platform.
#ifndef __DREAMCAST__
tAudioBackend_stream* AudioBackend_StreamOpen(int bit_depth, int channels, unsigned int sample_rate) {
    tMiniaudio_stream* new;
    ma_data_converter_config data_converter_config;

    new = malloc(sizeof(tMiniaudio_stream));
    new->sample_rate = sample_rate;
    new->total_frames_pushed = 0;
    new->total_frames_freed = 0;
    new->throttling = 0;
    ma_format format;
    switch (bit_depth) {
    case 8:
        format = ma_format_u8;
        new->frame_size_in_bytes = 1 * channels;
        break;
    case 16:
        format = ma_format_s16;
        new->frame_size_in_bytes = 2 * channels;
        break;
    case 24:
        format = ma_format_s24;
        new->frame_size_in_bytes = 3 * channels;
        break;
    case 32:
        format = ma_format_s32;
        new->frame_size_in_bytes = 4 * channels;
        break;
    default:
        goto failed;
    }

    if ((new->frame_size_in_bytes == 0) || (ma_paged_audio_buffer_data_init(format, channels, &new->paged_audio_buffer_data) != MA_SUCCESS)) {
        LOG_WARN("Failed to create paged audio buffer data");
        goto failed;
    }

    ma_paged_audio_buffer_config paged_audio_buffer_config = ma_paged_audio_buffer_config_init(&new->paged_audio_buffer_data);
    if (ma_paged_audio_buffer_init(&paged_audio_buffer_config, &new->paged_audio_buffer) != MA_SUCCESS) {
        LOG_WARN("Failed to create paged audio buffer for smacker audio stream");
        goto failed;
    }

    if (ma_sound_init_from_data_source(&engine, &new->paged_audio_buffer, MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION, NULL, NULL, &new->sound) != MA_SUCCESS) {
        LOG_WARN("Failed to create sound from data source");
        goto failed;
    }

    // allocate and initialize data converter if miniaudio engine and Smack file soundtrack sample rates differ
    if (ma_engine_get_sample_rate(&engine) != sample_rate) {
        new->needs_converting = 1;
        data_converter_config = ma_data_converter_config_init(format, format, channels, channels, sample_rate, ma_engine_get_sample_rate(&engine));
        if (ma_data_converter_init(&data_converter_config, NULL, &new->data_converter) != MA_SUCCESS) {
            LOG_WARN("Failed to create sound data converter");
            goto failed;
        }
    }
    return new;

failed:
    free(new);
    return NULL;
}

// Frees pages from the front of the list once playback has moved past them
// entirely. Safe to call from the same thread that calls StreamWrite/append:
// the mixer thread only ever reads pCurrent and follows pCurrent->pNext
// forward, it never re-touches pData->head.pNext once playback has started
// (the only other code that does, ma_paged_audio_buffer_seek_to_pcm_frame,
// is also only ever called from here, on this same thread, never
// concurrently with this). Never frees the page the mixer is currently on.
static void AudioBackend_TrimConsumedPages(tMiniaudio_stream* stream) {
    ma_uint64 played_cursor;
    ma_paged_audio_buffer_page* page;

    if (ma_paged_audio_buffer_get_cursor_in_pcm_frames(&stream->paged_audio_buffer, &played_cursor) != MA_SUCCESS) {
        return;
    }

    for (;;) {
        page = (ma_paged_audio_buffer_page*)ma_atomic_load_ptr(&stream->paged_audio_buffer_data.head.pNext);
        if (page == NULL || page == stream->paged_audio_buffer.pCurrent) {
            break;
        }
        if (stream->total_frames_freed + page->sizeInFrames > played_cursor) {
            break;
        }

        ma_atomic_store_ptr(&stream->paged_audio_buffer_data.head.pNext, ma_atomic_load_ptr(&page->pNext));
        stream->total_frames_freed += page->sizeInFrames;
        ma_paged_audio_buffer_data_free_page(&stream->paged_audio_buffer_data, page, NULL);
    }
}

tAudioBackend_error_code AudioBackend_StreamWrite(void* stream_handle, const unsigned char* data, unsigned long size) {
    tMiniaudio_stream* stream = stream_handle;
    ma_uint64 nb_frames_in;
    ma_uint64 nb_frames_out;
    ma_uint64 current_pos;
    ma_paged_audio_buffer_page* new_page;
    int skip_push;
    // Temporary diagnostics: this call is the prime suspect for the Smacker
    // cutscene cracking/slowdown that grows worse the longer a video plays.
    // Print only on a new worst-case duration, so this stays quiet unless
    // something is actually degrading, and shows exactly when/how it does.
    static int g_diag_audio_restarts = 0;
    static unsigned int g_diag_max_ms = 0;
    static int g_diag_call_count = 0;
    unsigned int _diag_t0 = gHarness_platform.GetTicks();
    g_diag_call_count++;

    // Ground-truth check, independent of our own paged-buffer bookkeeping:
    // does the engine's own clock (driven purely by the device's background
    // mixer thread pulling periods, nothing to do with our code) advance at
    // all on real hardware? If this stays frozen too, the problem is the
    // device thread itself never running/advancing, not anything in our
    // trimming/seeking logic.
    {
        static unsigned int g_diag_engine_t0_ticks = 0;
        static ma_uint64 g_diag_engine_t0_frames = (ma_uint64)-1;
        unsigned int now_ticks = gHarness_platform.GetTicks();
        ma_uint64 now_engine_frames = ma_engine_get_time_in_pcm_frames(&engine);
        if (g_diag_engine_t0_frames == (ma_uint64)-1) {
            g_diag_engine_t0_ticks = now_ticks;
            g_diag_engine_t0_frames = now_engine_frames;
        } else if ((g_diag_call_count % 50) == 0) {
            // ma_engine's clock advances on every device callback regardless
            // of whether any sound is actually attached/mixed - it only
            // proves the device thread is alive, not that THIS sound is
            // being read. Compare against the sound's own tracked cursor
            // (ma_sound_get_cursor_in_pcm_frames, which goes through
            // pSound->pDataSource - the same struct as our manual paged
            // buffer query) to see whether they actually agree.
            ma_uint64 sound_cursor = (ma_uint64)-1;
            ma_uint64 raw_cursor = (ma_uint64)-1;
            ma_sound_get_cursor_in_pcm_frames(&stream->sound, &sound_cursor);
            ma_paged_audio_buffer_get_cursor_in_pcm_frames(&stream->paged_audio_buffer, &raw_cursor);
            printf("[engine-diag] wall_ms=%u engine_frames_advanced=%llu (engine_rate=%u) sound_cursor=%llu raw_cursor=%llu\n",
                now_ticks - g_diag_engine_t0_ticks,
                (unsigned long long)(now_engine_frames - g_diag_engine_t0_frames),
                ma_engine_get_sample_rate(&engine),
                (unsigned long long)sound_cursor, (unsigned long long)raw_cursor);
        }
    }

    current_pos = stream->total_frames_pushed;

    // Backpressure: cap how far the resident (pushed but not yet played)
    // backlog is allowed to grow. On real hardware the backlog grows
    // steadily even though pushes are correctly real-time-paced (decode is
    // well under its frame budget) and this never happens on Flycast - that
    // points at the AICA's actual playback clock running slightly off from
    // the nominal sample rate we compute against, not a software pacing bug.
    // Rather than chase that hardware constant, bound the damage: if we're
    // already over the cap, skip *pushing new data* this call instead of
    // growing the backlog further - but still fall through to the restart
    // and trim logic below. An earlier version of this returned early here,
    // skipping the restart check too - once skipping had gone on long enough
    // that the sound genuinely exhausted its backlog and stopped
    // (ma_sound_is_playing() goes false, confirmed via diagnostics:
    // "playing=0 atend=1"), that early return meant it could NEVER be
    // restarted again, since the restart check was unreachable from here -
    // permanent silence, just delayed until the cap was first hit instead of
    // happening on the very first push. Skipping the push must never skip
    // restarting playback of whatever's already buffered.
    AudioBackend_TrimConsumedPages(stream);
    {
        ma_uint32 native_rate = ma_engine_get_sample_rate(&engine);
        ma_uint64 high_watermark = (ma_uint64)native_rate * 4; // 4s
        ma_uint64 low_watermark = (ma_uint64)native_rate * 1;  // 1s
        ma_uint64 resident = stream->total_frames_pushed - stream->total_frames_freed;
        // Hysteresis, not a single threshold: push genuinely outpaces drain
        // by a small, sustained amount on real hardware (consistent with
        // the AICA's actual playback clock running slightly differently
        // from the nominal rate we compute against) - a single-threshold
        // check just dips under the cap for one call, lets a push through
        // immediately, and the backlog resumes its slow climb. Confirmed via
        // diagnostics: resident grew far past the cap over many seconds
        // despite that check technically being in place. Latching into a
        // sustained throttle until the backlog drains to a much lower
        // watermark actually claws it back down instead of merely grazing
        // the cap once and moving on.
        if (!stream->throttling && resident > high_watermark) {
            stream->throttling = 1;
        } else if (stream->throttling && resident <= low_watermark) {
            stream->throttling = 0;
        }
        // Only skip while the sound is still actively draining a healthy
        // backlog - if it has already run dry (not playing: either never
        // started, or just hit its end), skipping is self-defeating: with no
        // new page ever arriving, the mixer's pCurrent can never advance
        // past the exhausted page, TrimConsumedPages can never free it
        // (correctly refuses to free the page currently in use), resident
        // can never drop, and skip_push can never become false again - a
        // permanent deadlock no amount of restarting/seeking can escape.
        // Confirmed via diagnostics: cursor frozen at the exact same value
        // across many seconds of wall time even after fixing the seek
        // ordering bug below, because that fix could never get a chance to
        // matter while stuck in this state.
        skip_push = stream->throttling && ma_sound_is_playing(&stream->sound);
        if (skip_push) {
            static int g_diag_skip_count = 0;
            static unsigned long g_diag_last_freed_seen = (unsigned long)-1;
            g_diag_skip_count++;
            if (stream->total_frames_freed != g_diag_last_freed_seen) {
                g_diag_last_freed_seen = (unsigned long)stream->total_frames_freed;
                printf("[audio-diag] skip #%d pushed=%llu freed=%llu resident=%llu playing=%d atend=%d\n",
                    g_diag_skip_count, (unsigned long long)stream->total_frames_pushed,
                    (unsigned long long)stream->total_frames_freed, (unsigned long long)resident,
                    ma_sound_is_playing(&stream->sound), ma_sound_at_end(&stream->sound));
            }
        }
    }

    // do we need to convert the sample frequency?
    if (skip_push) {
        // Nothing - over the backlog cap, don't grow it further this call.
    } else if (stream->needs_converting) {
        nb_frames_in = size / stream->frame_size_in_bytes;
        nb_frames_out = nb_frames_in * ma_engine_get_sample_rate(&engine) / stream->sample_rate;

        if (ma_paged_audio_buffer_data_allocate_page(&stream->paged_audio_buffer_data, nb_frames_out, NULL, NULL, &new_page) != MA_SUCCESS) {
            LOG_WARN("ma_paged_audio_buffer_data_allocate_page failed");
            return eAB_error;
        }
        if (ma_data_converter_process_pcm_frames(&stream->data_converter, data, &nb_frames_in, new_page->pAudioData, &nb_frames_out) != MA_SUCCESS) {
            LOG_WARN("ma_data_converter_process_pcm_frames failed");
            return eAB_error;
        }
        if (ma_paged_audio_buffer_data_append_page(&stream->paged_audio_buffer_data, new_page) != MA_SUCCESS) {
            LOG_WARN("ma_paged_audio_buffer_data_append_page failed");
            return eAB_error;
        }
        stream->total_frames_pushed += nb_frames_out;
    } else { // no sampling frequency conversion needed
        ma_uint32 frames_this_write = (ma_uint32)(size / (ma_uint64)stream->frame_size_in_bytes);
        if (ma_paged_audio_buffer_data_allocate_and_append_page(&stream->paged_audio_buffer_data, frames_this_write, data, NULL) != MA_SUCCESS) {
            LOG_WARN("ma_paged_audio_buffer_data_allocate_and_append_page failed");
            return eAB_error;
        }
        stream->total_frames_pushed += frames_this_write;
    }

    // Always check whether playback needs (re)starting, even if we just
    // skipped pushing new data above - there may still be unplayed backlog
    // queued from earlier calls that just needs the sound restarted to keep
    // draining (this is exactly what was missing before: see the long
    // comment above the cap check).
    if (!ma_sound_is_playing(&stream->sound)) {
        g_diag_audio_restarts++;
        // ma_sound_start() itself seeks to PCM frame 0 and clears the atEnd
        // flag whenever the sound is currently at-end (miniaudio.h, inside
        // ma_sound_start(): "If the sound is at the end it means we want to
        // start from the start again"). That's correct for one-shot file
        // playback but wrong for us: it unconditionally overwrites whatever
        // position we seek to *beforehand*, every single restart. Calling
        // start() before our own seek means OUR seek is the one that sticks.
        if (ma_sound_start(&stream->sound) != MA_SUCCESS) {
            LOG_WARN("ma_sound_start failed");
        }
        // seek either at start, or where the accumulated value hasn't played yet
        //
        // ma_paged_audio_buffer_seek_to_pcm_frame() walks the page list treating
        // its *current* head as position 0 - it has no idea pages have been
        // trimmed off the front by AudioBackend_TrimConsumedPages. current_pos
        // is an absolute count since the stream began, so once any trimming has
        // happened it overshoots by exactly total_frames_freed, the seek walks
        // off the end of the (now-shorter) list, returns MA_BAD_SEEK and leaves
        // the cursor untouched - silently desyncing it from reality on every
        // restart from then on. Translate to the list-relative position instead.
        if (ma_sound_seek_to_pcm_frame(&stream->sound, current_pos - stream->total_frames_freed) != MA_SUCCESS) {
            LOG_WARN("ma_sound_seek_to_pcm_frame failed");
        }
    }
    if (ma_sound_at_end(&stream->sound)) {
        LOG_WARN("video not playing fast enough: sound starved!");
    }

    AudioBackend_TrimConsumedPages(stream);

    if ((g_diag_call_count % 100) == 0) {
        printf("[audio-diag] tick %d pushed=%llu freed=%llu resident=%llu\n",
            g_diag_call_count, (unsigned long long)stream->total_frames_pushed,
            (unsigned long long)stream->total_frames_freed,
            (unsigned long long)(stream->total_frames_pushed - stream->total_frames_freed));
    }

    {
        unsigned int _diag_dt = gHarness_platform.GetTicks() - _diag_t0;
        if (_diag_dt > g_diag_max_ms) {
            g_diag_max_ms = _diag_dt;
            printf("[audio-diag] StreamWrite new max %ums pushed=%llu freed=%llu restarts=%d\n",
                g_diag_max_ms, (unsigned long long)stream->total_frames_pushed,
                (unsigned long long)stream->total_frames_freed, g_diag_audio_restarts);
        }
    }
    return eAB_success;
}

tAudioBackend_error_code AudioBackend_StreamClose(tAudioBackend_stream* stream_handle) {
    tMiniaudio_stream* stream = stream_handle;
    if (stream->needs_converting) {
        ma_data_converter_uninit(&stream->data_converter, NULL);
    }
    ma_sound_stop(&stream->sound);
    ma_sound_uninit(&stream->sound);
    ma_paged_audio_buffer_uninit(&stream->paged_audio_buffer);
    ma_paged_audio_buffer_data_uninit(&stream->paged_audio_buffer_data, NULL);

    free(stream);
    return eAB_success;
}
#endif // !__DREAMCAST__
