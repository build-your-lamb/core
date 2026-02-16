#ifndef LIVEKIT_H_
#define LIVEKIT_H_
#include <stdlib.h>
#include <stdint.h> // For uint8_t

#define LIVEKIT_VIDEO_WIDTH 640
#define LIVEKIT_VIDEO_HEIGHT 480

int app_livekit_main();

void app_livekit_quit();

#endif  // LIVEKIT_H_
