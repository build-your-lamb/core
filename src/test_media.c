#include <stdio.h>
#include "media.h"
#include "utils.h" // Assuming LOG macros will be used
#include "media.h"
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

#include <stdio.h>
#include "media.h"
#include "utils.h" // Assuming LOG macros will be used
#include "media.h"
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>

#define TEST_VIDEO_FILE "test_video.h264"
// opus
#define TEST_AUDIO_FILE "test_audio.opus"

#define VIDEO_BUFFER_SIZE (256 * 1024) // 256 KB for video frames
#define AUDIO_BUFFER_SIZE (4 * 1024)   // 4 KB for audio frames

static FILE* video_file = NULL;
static FILE* audio_file = NULL;
static nng_socket video_sock_global; // Using global for cleanup
static nng_socket audio_sock_global; // Using global for cleanup


int test_media_read_video_buf(char* buf, size_t* size) {
  if (video_file == NULL) {
	      video_file = fopen(TEST_VIDEO_FILE, "rb");
	      if (video_file == NULL) {
	          LOGE("Failed to open video file: %s", TEST_VIDEO_FILE);
	          *size = 0;
            return -1; // Indicate error
	      }
  }

  *size = fread(buf, 1, VIDEO_BUFFER_SIZE, video_file);
  if (*size == 0) {
      if (feof(video_file)) {
          LOGI("End of video file. Looping.");
          fseek(video_file, 0, SEEK_SET); // Loop back to start
          *size = fread(buf, 1, VIDEO_BUFFER_SIZE, video_file);
      }
  }
  return (*size > 0) ? 0 : -1; // Return 0 on success, -1 on failure
}

int test_media_read_audio_buf(char* buf, size_t* size) {
  if (audio_file == NULL) {
	      audio_file = fopen(TEST_AUDIO_FILE, "rb");
	      if (audio_file == NULL) {
	          LOGE("Failed to open audio file: %s", TEST_AUDIO_FILE);
	          *size = 0;
            return -1; // Indicate error
	      }
  }

  *size = fread(buf, 1, AUDIO_BUFFER_SIZE, audio_file);
  if (*size == 0) {
      if (feof(audio_file)) {
          LOGI("End of audio file. Looping.");
          fseek(audio_file, 0, SEEK_SET); // Loop back to start
          *size = fread(buf, 1, AUDIO_BUFFER_SIZE, audio_file);
      }
  }
  return (*size > 0) ? 0 : -1; // Return 0 on success, -1 on failure
}

int app_test_media_main(void) {
  int rv;
  // create topic MEDIA_TOPIC_VIDEO_COMPRESSED and MEDIA_TOPIC_AUDIO_COMPRESSED
  // already define in media.h
  // create a PUB socket
  if ((rv = nng_pub0_open(&video_sock_global)) != 0) {
    LOGE("Failed to open video socket: %s", nng_strerror(rv));
    return rv;
  } 

  if ((rv = nng_pub0_open(&audio_sock_global)) != 0) {
    LOGE("Failed to open audio socket: %s", nng_strerror(rv));
    nng_close(video_sock_global);
    return rv;
  }

  // bind the socket to an address
  if ((rv = nng_listen(video_sock_global, MEDIA_TOPIC_VIDEO_COMPRESSED, NULL, 0)) != 0) {
    LOGE("Failed to listen on video socket: %s", nng_strerror(rv));
    nng_close(video_sock_global);
    nng_close(audio_sock_global);
    return rv;
  }

  if ((rv = nng_listen(audio_sock_global, MEDIA_TOPIC_AUDIO_COMPRESSED, NULL, 0)) != 0) {
    LOGE("Failed to listen on audio socket: %s", nng_strerror(rv));
    nng_close(video_sock_global);
    nng_close(audio_sock_global);
    return rv;
  }

  // readfile and publish the data to the socket
  char video_buf[VIDEO_BUFFER_SIZE];
  char audio_buf[AUDIO_BUFFER_SIZE];
  size_t video_size;
  size_t audio_size;
  nng_msg* video_msg;
  nng_msg* audio_msg;


  while (1) {
    // Read and publish video
    if (test_media_read_video_buf(video_buf, &video_size) == 0 && video_size > 0) {
        if ((rv = nng_msg_alloc(&video_msg, 0)) != 0) {
            LOGE("Failed to allocate video message: %s", nng_strerror(rv));
        } else {
            nng_msg_append(video_msg, video_buf, video_size);
            if ((rv = nng_sendmsg(video_sock_global, video_msg, NNG_FLAG_NONBLOCK)) != 0) {
                LOGE("Failed to send video message: %s", nng_strerror(rv));
                nng_msg_free(video_msg); // Free message if send fails
            }
        }
    }

    // Read and publish audio
    if (test_media_read_audio_buf(audio_buf, &audio_size) == 0 && audio_size > 0) {
        if ((rv = nng_msg_alloc(&audio_msg, 0)) != 0) {
            LOGE("Failed to allocate audio message: %s", nng_strerror(rv));
        } else {
            nng_msg_append(audio_msg, audio_buf, audio_size);
            if ((rv = nng_sendmsg(audio_sock_global, audio_msg, NNG_FLAG_NONBLOCK)) != 0) {
                LOGE("Failed to send audio message: %s", nng_strerror(rv));
                nng_msg_free(audio_msg); // Free message if send fails
            }
            // nng_msg_free(audio_msg); // No need to free here, nng_sendmsg handles it
        }
    }

    nng_msleep(20); // Publish approximately at 50fps (20ms delay)
  }

  // Should not reach here in current infinite loop, but for completeness
  return 0;
}

// Function to quit the media test application
void app_test_media_quit(void) {
    LOGI("Calling app_media_quit() from quit_media_tests().");
    if (video_file != NULL) {
        fclose(video_file);
        video_file = NULL;
    }
    if (audio_file != NULL) {
        fclose(audio_file);
        audio_file = NULL;
    }
    // Close NNG sockets
    nng_close(video_sock_global);
    nng_close(audio_sock_global);
    LOGI("quit_media_tests completed.");
}

int app_media_main(void) {
  return app_test_media_main();
}

void app_media_quit(void) {
  app_test_media_quit();
}
