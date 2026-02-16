#ifndef MEDIA_H_
#define MEDIA_H_

#include <stdint.h>

// Media topic definitions
#define MEDIA_TOPIC_VIDEO_COMPRESSED "video/compressed"
#define MEDIA_TOPIC_AUDIO_COMPRESSED "audio/compressed"

// Function pointers for media frame callbacks
extern void (*media_on_video_frame)(uint8_t* data, int size);
extern void (*media_on_audio_frame)(uint8_t* data, int size);

int app_media_main();
void app_media_quit();

#endif // MEDIA_H_
