#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>

#define DEFAULT_AUDIO_DEVICE "default"
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_CHANNELS 1
#define DEFAULT_FRAME_SIZE_MS 20
#define DEFAULT_BITRATE 32000 // 32 kbps

/**
 * @brief Main function for the audio capture and publishing module.
 *        Initializes ALSA, Opus encoder, NNG publisher and starts the capture thread.
 *
 * @param arg Unused, but required by app_main_func_t signature.
 * @return 0 on successful operation, 1 on failure.
 */
int app_audio_main(void *arg);

/**
 * @brief Stops the audio capture and publishing thread and cleans up resources.
 */
void app_audio_quit();

#endif // AUDIO_H
