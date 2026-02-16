#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include "utils.h"
#define FPS 30
static int g_video_size = 0;
static uint8_t* g_video_buf = NULL;
static uint8_t* g_pps_buf = NULL;
static uint8_t* g_sps_buf = NULL;
static const uint32_t nalu_start_4bytecode = 0x01000000;
static const uint32_t nalu_start_3bytecode = 0x010000;

typedef enum H264_NALU_TYPE {
  NALU_TYPE_SPS = 7,
  NALU_TYPE_PPS = 8,
  NALU_TYPE_IDR = 5,
  NALU_TYPE_NON_IDR = 1,
} H264_NALU_TYPE;

int video_init() {
  FILE* video_fp = NULL;
  char videofile[] = "test.264";

  video_fp = fopen(videofile, "rb");

  if (video_fp == NULL) {
    printf("open file %s failed\n", videofile);
    return -1;
  }

  fseek(video_fp, 0, SEEK_END);
  g_video_size = ftell(video_fp);
  fseek(video_fp, 0, SEEK_SET);
  g_video_buf = (uint8_t*)calloc(1, g_video_size);
  fread(g_video_buf, g_video_size, 1, video_fp);
  fclose(video_fp);
  return 0;
}

uint8_t* video_h264_find_nalu(uint8_t* buf_start, uint8_t* buf_end) {
  uint8_t* p = buf_start;

  while ((p + 3) < buf_end) {
    if (memcmp(p, &nalu_start_4bytecode, 4) == 0) {
      return p;
    } else if (memcmp(p, &nalu_start_3bytecode, 3) == 0) {
      return p;
    }
    p++;
  }

  return buf_end;
}

uint8_t* video_get_video_frame(int* size) {
  uint8_t* buf = NULL;
  static int pps_size = 0;
  static int sps_size = 0;
  uint8_t* buf_end = g_video_buf + g_video_size;

  static uint8_t* pstart = NULL;
  static uint8_t* pend = NULL;
  size_t nalu_size;

  if (!pstart)
    pstart = g_video_buf;

  pend = video_h264_find_nalu(pstart + 2, buf_end);

  if (pend == buf_end) {
    pstart = NULL;
    return NULL;
  }

  nalu_size = pend - pstart;
  int start_code_offset = memcmp(pstart, &nalu_start_3bytecode, 3) == 0 ? 3 : 4;
  H264_NALU_TYPE nalu_type = (H264_NALU_TYPE)(pstart[start_code_offset] & 0x1f);
  switch (nalu_type) {
    case NALU_TYPE_SPS:
      sps_size = nalu_size;
      if (g_sps_buf != NULL) {
        free(g_sps_buf);
        g_sps_buf = NULL;
      }
      g_sps_buf = (uint8_t*)calloc(1, sps_size);
      memcpy(g_sps_buf, pstart, sps_size);
      break;
    case NALU_TYPE_PPS:
      pps_size = nalu_size;
      if (g_pps_buf != NULL) {
        free(g_pps_buf);
        g_pps_buf = NULL;
      }
      g_pps_buf = (uint8_t*)calloc(1, pps_size);
      memcpy(g_pps_buf, pstart, pps_size);

      break;
    case NALU_TYPE_IDR:
      *size = sps_size + pps_size + nalu_size;
      buf = (uint8_t*)calloc(1, *size);
      memcpy(buf, g_sps_buf, sps_size);
      memcpy(buf + sps_size, g_pps_buf, pps_size);
      memcpy(buf + sps_size + pps_size, pstart, nalu_size);

      break;
    case NALU_TYPE_NON_IDR:
    default:
      *size = nalu_size;
      buf = (uint8_t*)calloc(1, *size);
      memcpy(buf, pstart, nalu_size);

      break;
  }

  pstart = pend;

  return buf;
}

void video_deinit() {
  if (g_sps_buf != NULL) {
    free(g_sps_buf);
    g_sps_buf = NULL;
  }

  if (g_pps_buf != NULL) {
    free(g_pps_buf);
    g_pps_buf = NULL;
  }

  if (g_video_buf != NULL) {
    free(g_video_buf);
    g_video_buf = NULL;
  }
}

int app_video_main() {
  if (video_init() < 0) {
    return -1;
  }

  int frame_size = 0;
  uint8_t* frame_buf = NULL;

  nng_socket sock;
  nng_pub0_open(&sock);
  nng_listen(sock, "inproc://video.compressed", NULL, 0);
  while (1) {

    if ((frame_buf = video_get_video_frame(&frame_size)) != NULL) {
      //printf("get a video frame, size: %d [%.2x %.2x %.2x %.2x]\n", frame_size, frame_buf[0], frame_buf[1], frame_buf[2], frame_buf[3]);
      nng_send(sock, frame_buf, frame_size, 0);
      free(frame_buf); 
    }
    usleep(1000000 / FPS);
  }

  video_deinit();
  return 0;
}

void app_video_quit() {
}
