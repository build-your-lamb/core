#ifndef VIDEO_H_
#define VIDEO_H_
#include <stdio.h>

#define TOPIC_VIDEO_COMPRESSED "inproc://video.compressed"
#define TOPIC_VIDEO_RAW "inproc://video.raw"
#define TOPIC_VIDEO_WEBRTC "inproc://video.webrtc"

int app_video_main(void *arg);

void app_video_set_pipelines(const char *cam_pipeline,
                             const char *dis_pipeline);

void app_video_quit();

#endif // VIDEO_H_
