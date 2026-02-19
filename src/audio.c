#include "audio.h"
#include "utils.h" // For LOGE, LOGI

#include <alsa/asoundlib.h>
#include <opus/opus.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h> // For usleep

#define TOPIC_AUDIO_COMPRESSED "inproc://audio.compressed"
#define MAX_FRAME_SIZE 6 * 48000 / 1000 * 2 // 6ms * 48kHz * 2 bytes/sample (stereo) * 2 (for safety)

static snd_pcm_t *g_alsa_handle = NULL;
static OpusEncoder *g_opus_encoder = NULL;
static nng_socket g_nng_audio_sock;
static pthread_t g_audio_thread;
static volatile bool g_running = false;

// Audio parameters
static const char *g_audio_device = DEFAULT_AUDIO_DEVICE;
static int g_sample_rate = DEFAULT_SAMPLE_RATE;
static int g_channels = DEFAULT_CHANNELS;
static int g_frame_size_ms = DEFAULT_FRAME_SIZE_MS;
static int g_bitrate = DEFAULT_BITRATE;
static int g_frame_size_samples; // Number of samples per frame
static int g_frame_size_bytes;   // Number of bytes per frame

static void *audio_capture_thread(void *arg) {
    int rv;
    int opus_encode_error;
    int buffer_size;
    
    // Calculate frame size in samples and bytes
    g_frame_size_samples = (g_sample_rate / 1000) * g_frame_size_ms;
    g_frame_size_bytes = g_frame_size_samples * g_channels * sizeof(int16_t);

    int16_t *pcm_buffer = (int16_t *)malloc(g_frame_size_bytes);
    if (!pcm_buffer) {
        LOGE("audio_capture_thread: Failed to allocate PCM buffer.");
        g_running = false;
        return NULL;
    }

    uint8_t *opus_buffer = (uint8_t *)malloc(MAX_FRAME_SIZE);
    if (!opus_buffer) {
        LOGE("audio_capture_thread: Failed to allocate Opus buffer.");
        free(pcm_buffer);
        g_running = false;
        return NULL;
    }

    LOGI("Audio capture thread started. Device: %s, Rate: %d, Ch: %d, Frame_ms: %d, Bitrate: %d",
         g_audio_device, g_sample_rate, g_channels, g_frame_size_ms, g_bitrate);

    while (g_running) {
        // Read PCM data from ALSA
        rv = snd_pcm_readi(g_alsa_handle, pcm_buffer, g_frame_size_samples);
        if (rv == -EPIPE) {
            // EPIPE means overrun
            LOGW("audio_capture_thread: ALSA overrun, attempting to recover.");
            snd_pcm_prepare(g_alsa_handle);
            continue;
        } else if (rv < 0) {
            LOGE("audio_capture_thread: Error from ALSA read: %s", snd_strerror(rv));
            break;
        } else if (rv != g_frame_size_samples) {
            LOGW("audio_capture_thread: Short read from ALSA, expected %d samples, got %d",
                 g_frame_size_samples, rv);
            // Optionally, fill remaining buffer with zeros or handle as needed.
        }

        // Encode PCM data to Opus
        int opus_len = opus_encode(g_opus_encoder, pcm_buffer, g_frame_size_samples,
                                   opus_buffer, MAX_FRAME_SIZE);
        if (opus_len < 0) {
            LOGE("audio_capture_thread: Opus encode error: %s", opus_strerror(opus_len));
            break;
        }

        // Publish Opus data via NNG
        nng_msg *msg;
        if ((rv = nng_msg_alloc(&msg, opus_len)) != 0) {
            LOGE("audio_capture_thread: nng_msg_alloc error: %s", nng_strerror(rv));
            break;
        }
        memcpy(nng_msg_body(msg), opus_buffer, opus_len);

        if ((rv = nng_sendmsg(g_nng_audio_sock, msg, 0)) != 0) {
            LOGE("audio_capture_thread: nng_sendmsg error: %s", nng_strerror(rv));
            nng_msg_free(msg);
            break;
        }
        // LOGD("Sent Opus packet of size %d", opus_len); // For debugging, enable if needed
    }

    LOGI("Audio capture thread stopped.");
    free(pcm_buffer);
    free(opus_buffer);
    return NULL;
}

int app_audio_main(void *arg) {
    (void)arg; // Arg is unused, but required by signature

    int err;

    // ALSA Initialization
    snd_pcm_hw_params_t *hw_params;
    if ((err = snd_pcm_open(&g_alsa_handle, g_audio_device, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        LOGE("app_audio_main: Cannot open audio device %s: %s", g_audio_device, snd_strerror(err));
        return 1;
    }

    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(g_alsa_handle, hw_params);

    snd_pcm_hw_params_set_access(g_alsa_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(g_alsa_handle, hw_params, SND_PCM_FORMAT_S16_LE); // Signed 16-bit Little Endian
    snd_pcm_hw_params_set_channels(g_alsa_handle, hw_params, g_channels);
    unsigned int actual_rate = g_sample_rate;
    snd_pcm_hw_params_set_rate_near(g_alsa_handle, hw_params, &actual_rate, 0);
    if (actual_rate != g_sample_rate) {
        LOGW("app_audio_main: Sample rate mismatch. Requested %d, got %d", g_sample_rate, actual_rate);
        g_sample_rate = actual_rate;
    }

    snd_pcm_uframes_t period_size = (g_sample_rate / 1000) * g_frame_size_ms;
    snd_pcm_hw_params_set_period_size_near(g_alsa_handle, hw_params, &period_size, 0);
    LOGI("app_audio_main: ALSA period size set to %lu frames", period_size);

    if ((err = snd_pcm_hw_params(g_alsa_handle, hw_params)) < 0) {
        LOGE("app_audio_main: Cannot set ALSA hardware parameters: %s", snd_strerror(err));
        app_audio_quit(); // Use new quit function for cleanup
        return 1;
    }

    if ((err = snd_pcm_prepare(g_alsa_handle)) < 0) {
        LOGE("app_audio_main: Cannot prepare ALSA audio interface: %s", snd_strerror(err));
        app_audio_quit(); // Use new quit function for cleanup
        return 1;
    }

    LOGI("app_audio_main: ALSA initialized successfully.");

    // Opus Encoder Initialization
    int opus_error;
    g_opus_encoder = opus_encoder_create(g_sample_rate, g_channels, OPUS_APPLICATION_VOIP, &opus_error);
    if (opus_error < 0) {
        LOGE("app_audio_main: Failed to create Opus encoder: %s", opus_strerror(opus_error));
        app_audio_quit(); // Use new quit function for cleanup
        return 1;
    }
    opus_encoder_ctl(g_opus_encoder, OPUS_SET_BITRATE(g_bitrate));
    opus_encoder_ctl(g_opus_encoder, OPUS_SET_VBR(0)); // Constant Bit Rate
    opus_encoder_ctl(g_opus_encoder, OPUS_SET_COMPLEXITY(10)); // Max complexity

    LOGI("app_audio_main: Opus encoder initialized successfully.");

    // NNG Publisher Initialization
    if ((err = nng_pub0_open(&g_nng_audio_sock)) != 0) {
        LOGE("app_audio_main: nng_pub0_open for audio: %s", nng_strerror(err));
        app_audio_quit(); // Use new quit function for cleanup
        return 1;
    }

    if ((err = nng_listen(g_nng_audio_sock, TOPIC_AUDIO_COMPRESSED, NULL, 0)) != 0) {
        LOGE("app_audio_main: nng_listen for audio: %s", nng_strerror(err));
        app_audio_quit(); // Use new quit function for cleanup
        return 1;
    }
    LOGI("app_audio_main: NNG audio publisher initialized on %s.", TOPIC_AUDIO_COMPRESSED);

    // Start audio capture thread
    g_running = true;
    if (pthread_create(&g_audio_thread, NULL, audio_capture_thread, NULL) != 0) {
        LOGE("app_audio_main: Failed to create audio capture thread.");
        app_audio_quit(); // Use new quit function for cleanup
        return 1;
    }
    LOGI("app_audio_main: Audio capture thread created.");

    // This thread (app_audio_main) will just return, and the pthread_detach in start_app will handle it.
    // The actual audio capture happens in audio_capture_thread.
    return 0;
}

void app_audio_quit() {
    if (!g_running) {
        LOGW("app_audio_quit: Audio capture not running.");
        return;
    }
    g_running = false;
    pthread_join(g_audio_thread, NULL); // Wait for the thread to finish

    // Cleanup resources
    if (g_alsa_handle) {
        snd_pcm_close(g_alsa_handle);
        g_alsa_handle = NULL;
        LOGI("app_audio_quit: ALSA handle closed.");
    }
    if (g_opus_encoder) {
        opus_encoder_destroy(g_opus_encoder);
        g_opus_encoder = NULL;
        LOGI("app_audio_quit: Opus encoder destroyed.");
    }
    if (nng_socket_id(g_nng_audio_sock) != -1) {
        nng_close(g_nng_audio_sock);
        g_nng_audio_sock.id = -1;
        LOGI("app_audio_quit: NNG audio socket closed.");
    }
    LOGI("app_audio_quit: Audio capture stopped and resources cleaned up.");
}
