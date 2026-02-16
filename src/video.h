#ifndef VIDEO_H_
#define VIDEO_H_
#include <stdio.h>

#define TOPIC_VIDEO_COMPRESSED "inproc://video.compressed"
#define TOPIC_VIDEO_RAW "inproc://video.raw"

int app_video_main();

void app_video_quit();

#endif // VIDEO_H_
