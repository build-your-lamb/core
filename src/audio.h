#ifndef AUDIO_H
#define AUDIO_H

#include <stdbool.h>

#define DEFAULT_AUDIO_DEVICE "default"
#define DEFAULT_SAMPLE_RATE 48000
#define DEFAULT_CHANNELS 1
#define DEFAULT_FRAME_SIZE_MS 20
#define DEFAULT_BITRATE 32000 // 32 kbps

#define TOPIC_AUDIO_COMPRESSED "inproc://audio.compressed"
#define TOPIC_AUDIO_WEBRTC "inproc://audio.webrtc"

int app_audio_main(void *arg);
void app_audio_set_pipelines(const char *mic_pipeline,
                             const char *spk_pipeline);
void app_audio_quit(void);

#endif // AUDIO_H
