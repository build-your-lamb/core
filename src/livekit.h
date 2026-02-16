#ifndef LIVEKIT_H_
#define LIVEKIT_H_
#include <stdint.h> // For uint8_t
#include <stdlib.h>

// Structure to pass LiveKit specific arguments
typedef struct {
  const char *url;
  const char *token;
} LiveKitArgs;

#define LIVEKIT_VIDEO_WIDTH 640
#define LIVEKIT_VIDEO_HEIGHT 480

int app_livekit_main(void *arg);

void app_livekit_quit();

#endif // LIVEKIT_H_
