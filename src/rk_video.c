#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <time.h>
#include <unistd.h>

#include "rk_debug.h"
#include "rk_defines.h"
#include "rk_mpi_adec.h"
#include "rk_mpi_aenc.h"
#include "rk_mpi_ai.h"
#include "rk_mpi_ao.h"
#include "rk_mpi_avs.h"
#include "rk_mpi_cal.h"
#include "rk_mpi_ivs.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_rgn.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_tde.h"
#include "rk_mpi_vdec.h"
#include "rk_mpi_venc.h"
#include "rk_mpi_vi.h"
#include "rk_mpi_vo.h"
#include "rk_mpi_vpss.h"

#include "utils.h"

pthread_t main_thread;
void (*media_on_video_frame)(uint8_t *data, int size) = NULL;
void (*media_on_audio_frame)(uint8_t *data, int size) = NULL;
static int media_quit_flag = 0;
static pthread_mutex_t media_mutex = PTHREAD_MUTEX_INITIALIZER;
MPP_CHN_S stSrcChn, stDestChn;

static void *GetMediaBuffer0(void *arg) {
  (void)arg;
  printf("========%s========\n", __func__);
  void *pData = RK_NULL;
  int s32Ret;

  VENC_STREAM_S stFrame;
  stFrame.pstPack = malloc(sizeof(VENC_PACK_S));

  while (!media_quit_flag) {
    s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
    if (s32Ret == RK_SUCCESS) {
      pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
      // fwrite(pData, 1, stFrame.pstPack->u32Len, venc0_file);
      if (media_on_video_frame) {
        media_on_video_frame((uint8_t *)pData, stFrame.pstPack->u32Len);
      }

      s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
      if (s32Ret != RK_SUCCESS) {
        LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
      }
    } else {
      LOGE("RK_MPI_VI_GetChnFrame fail %x", s32Ret);
    }

    usleep(10 * 1000);
  }

  free(stFrame.pstPack);
  return NULL;
}

static RK_S32 test_venc_init(int chnId, int width, int height,
                             RK_CODEC_ID_E enType) {
  printf("========%s========\n", __func__);
  VENC_RECV_PIC_PARAM_S stRecvParam;
  VENC_CHN_ATTR_S stAttr;
  memset(&stAttr, 0, sizeof(VENC_CHN_ATTR_S));

  if (enType == RK_VIDEO_ID_AVC) {
    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
    stAttr.stRcAttr.stH264Cbr.u32BitRate = 10 * 1024;
    stAttr.stRcAttr.stH264Cbr.u32Gop = 60;
  } else if (enType == RK_VIDEO_ID_HEVC) {
    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
    stAttr.stRcAttr.stH265Cbr.u32BitRate = 10 * 1024;
    stAttr.stRcAttr.stH265Cbr.u32Gop = 60;
  } else if (enType == RK_VIDEO_ID_MJPEG) {
    stAttr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
    stAttr.stRcAttr.stMjpegCbr.u32BitRate = 10 * 1024;
  }

  stAttr.stVencAttr.enType = enType;
  stAttr.stVencAttr.enPixelFormat = RK_FMT_YUV420SP;
  if (enType == RK_VIDEO_ID_AVC)
    stAttr.stVencAttr.u32Profile = H264E_PROFILE_HIGH;
  stAttr.stVencAttr.u32PicWidth = width;
  stAttr.stVencAttr.u32PicHeight = height;
  stAttr.stVencAttr.u32VirWidth = width;
  stAttr.stVencAttr.u32VirHeight = height;
  stAttr.stVencAttr.u32StreamBufCnt = 2;
  stAttr.stVencAttr.u32BufSize = width * height * 3 / 2;
  stAttr.stVencAttr.enMirror = MIRROR_NONE;

  RK_MPI_VENC_CreateChn(chnId, &stAttr);

  // stRecvParam.s32RecvPicNum = 100;		//recv 100 slice
  // RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

  memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
  stRecvParam.s32RecvPicNum = -1;
  RK_MPI_VENC_StartRecvFrame(chnId, &stRecvParam);

  return 0;
}

// demo板dev默认都是0，根据不同的channel 来选择不同的vi节点
int vi_dev_init() {
  printf("%s\n", __func__);
  int ret = 0;
  int devId = 0;
  int pipeId = devId;

  VI_DEV_ATTR_S stDevAttr;
  VI_DEV_BIND_PIPE_S stBindPipe;
  memset(&stDevAttr, 0, sizeof(stDevAttr));
  memset(&stBindPipe, 0, sizeof(stBindPipe));
  // 0. get dev config status
  ret = RK_MPI_VI_GetDevAttr(devId, &stDevAttr);
  if (ret == RK_ERR_VI_NOT_CONFIG) {
    // 0-1.config dev
    ret = RK_MPI_VI_SetDevAttr(devId, &stDevAttr);
    if (ret != RK_SUCCESS) {
      printf("RK_MPI_VI_SetDevAttr %x\n", ret);
      return -1;
    }
  } else {
    printf("RK_MPI_VI_SetDevAttr already\n");
  }
  // 1.get dev enable status
  ret = RK_MPI_VI_GetDevIsEnable(devId);
  if (ret != RK_SUCCESS) {
    // 1-2.enable dev
    ret = RK_MPI_VI_EnableDev(devId);
    if (ret != RK_SUCCESS) {
      printf("RK_MPI_VI_EnableDev %x\n", ret);
      return -1;
    }
    // 1-3.bind dev/pipe
    stBindPipe.u32Num = 1;
    stBindPipe.PipeId[0] = pipeId;
    ret = RK_MPI_VI_SetDevBindPipe(devId, &stBindPipe);
    if (ret != RK_SUCCESS) {
      printf("RK_MPI_VI_SetDevBindPipe %x\n", ret);
      return -1;
    }
  } else {
    printf("RK_MPI_VI_EnableDev already\n");
  }

  return 0;
}

int vi_chn_init(int channelId, int width, int height) {
  int ret;
  int buf_cnt = 2;
  // VI init
  VI_CHN_ATTR_S vi_chn_attr;
  memset(&vi_chn_attr, 0, sizeof(vi_chn_attr));
  vi_chn_attr.stIspOpt.u32BufCount = buf_cnt;
  vi_chn_attr.stIspOpt.enMemoryType =
      VI_V4L2_MEMORY_TYPE_DMABUF; // VI_V4L2_MEMORY_TYPE_MMAP;
  vi_chn_attr.stSize.u32Width = width;
  vi_chn_attr.stSize.u32Height = height;
  vi_chn_attr.enPixelFormat = RK_FMT_YUV420SP;
  vi_chn_attr.enCompressMode = COMPRESS_MODE_NONE; // COMPRESS_AFBC_16x16;
  vi_chn_attr.u32Depth = 0; // 0, get fail, 1 - u32BufCount, can get, if bind to
                            // other device, must be < u32BufCount
  ret = RK_MPI_VI_SetChnAttr(0, channelId, &vi_chn_attr);
  ret |= RK_MPI_VI_EnableChn(0, channelId);
  if (ret) {
    printf("ERROR: create VI error! ret=%d\n", ret);
    return ret;
  }

  return ret;
}
/*
 * API functions for external use
 */
int media_init(void (*on_video_frame)(uint8_t *data, int size),
               void (*on_audio_frame)(uint8_t *data, int size)) {
  media_on_video_frame = on_video_frame;
  media_on_audio_frame = on_audio_frame;
  RK_MPI_SYS_Init();
}

void media_deinit() { RK_MPI_SYS_Exit(); }

int media_start() {
  pthread_mutex_lock(&media_mutex);
  RK_S32 s32Ret = RK_FAILURE;
  RK_U32 u32Width = 1280;
  RK_U32 u32Height = 720;
  RK_CODEC_ID_E enCodecType = RK_VIDEO_ID_AVC;
  RK_CHAR *pCodecName = "H264";
  RK_S32 s32chnlId = 0;
  int c;
  int ret = -1;

  printf("#CodecName:%s\n", pCodecName);
  printf("#Resolution: %dx%d\n", u32Width, u32Height);
  printf("#CameraIdx: %d\n\n", s32chnlId);

  vi_dev_init();
  vi_chn_init(s32chnlId, u32Width, u32Height);

  // venc  init
  test_venc_init(0, u32Width, u32Height,
                 enCodecType); // RK_VIDEO_ID_AVC RK_VIDEO_ID_HEVC

  // bind vi to venc
  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = 0;
  stSrcChn.s32ChnId = s32chnlId;

  stDestChn.enModId = RK_ID_VENC;
  stDestChn.s32DevId = 0;
  stDestChn.s32ChnId = 0;
  printf("====RK_MPI_SYS_Bind vi0 to venc0====\n");
  s32Ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
  if (s32Ret != RK_SUCCESS) {
    LOGE("bind 0 ch venc failed");
  }
  media_quit_flag = 0;
  pthread_create(&main_thread, NULL, GetMediaBuffer0, NULL);
  pthread_mutex_unlock(&media_mutex);
}

int media_stop() {
  pthread_mutex_lock(&media_mutex);
  media_quit_flag = 1;
  pthread_join(main_thread, NULL);

  int s32Ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
  if (s32Ret != RK_SUCCESS) {
    LOGE("RK_MPI_SYS_UnBind fail %x", s32Ret);
  }

  s32Ret = RK_MPI_VI_DisableChn(0, 0);
  LOGE("RK_MPI_VI_DisableChn %x", s32Ret);

  s32Ret = RK_MPI_VENC_StopRecvFrame(0);
  if (s32Ret != RK_SUCCESS) {
    pthread_mutex_unlock(&media_mutex);
    return s32Ret;
  }

  s32Ret = RK_MPI_VENC_DestroyChn(0);
  if (s32Ret != RK_SUCCESS) {
    LOGE("RK_MPI_VDEC_DestroyChn fail %x", s32Ret);
  }

  s32Ret = RK_MPI_VI_DisableDev(0);
  LOGE("RK_MPI_VI_DisableDev %x", s32Ret);
  s32Ret = 0;
  LOGE("test running exit:%d", s32Ret);
  pthread_mutex_unlock(&media_mutex);
  return s32Ret;
}

// Global flag to control the main loop of app_media_main
static volatile bool g_media_running = true;
// Global NNG socket for media publishing
static nng_socket g_media_nng_sock;

// Callback functions for media frames (for app_media_main to set)
static void on_video_frame_publish(uint8_t *data, int size);
static void on_audio_frame_publish(uint8_t *data, int size);

// Implementation of app_media_main declared in media.h
int app_media_main(void) {
  LOGI("Starting app_media_main.");

  // Initialize NNG publisher socket for media topics
  int rv;
  if ((rv = nng_pub0_open(&g_media_nng_sock)) != 0) {
    LOGE("nng_pub0_open for media: %s", nng_strerror(rv));
    return -1;
  }

  // Set up the NNG listen for media topics (this will be changed in Phase 2)
  // For now, let's just bind to a dummy address or the real one if known
  // Since we don't have the specific media topics yet, this will be a
  // placeholder. This part will be revisited in Phase 2 when topics are
  // defined. For now, let's use a placeholder. if ((rv =
  // nng_listen(g_media_nng_sock, "inproc://media_bus", NULL, 0)) != 0) {
  //     LOGE("nng_listen for media: %s", nng_strerror(rv));
  //     nng_close(g_media_nng_sock);
  //     return -1;
  // }

  // Initialize the underlying media system with callbacks to publish frames
  media_init(on_video_frame_publish, on_audio_frame_publish);
  media_start();

  // Call run_media_tests as requested by the user
  // This will be called once after media system is running
  // This part assumes run_media_tests will do its thing and return.
  // Include test_media.h for this.
#include "test_media.h" // Include here for now, will move to top later
  run_media_tests(); // User requested app_media_main calls app_test_media_main
                     // (now run_media_tests)

  // Main loop for app_media_main to keep running
  while (g_media_running) {
    // Here we could poll for events, sleep, or wait for signals
    // For now, a simple sleep to prevent busy-waiting
    sleep(1);
  }

  LOGI("Stopping app_media_main loop.");
  media_stop();
  media_deinit();
  nng_close(g_media_nng_sock); // Close the NNG socket for media
  LOGI("app_media_main exited.");
  return 0;
}

// Implementation of app_media_quit declared in media.h
void app_media_quit(void) {
  LOGI("Attempting to quit app_media_main.");
  g_media_running = false;
  // Potentially unblock any blocking calls in app_media_main if it were
  // blocking NNG socket is closed in app_media_main once its loop terminates
}

// Placeholder callback implementations (to be filled in Phase 2)
static void on_video_frame_publish(uint8_t *data, int size) {
  // LOGD("Video frame received, size: %d", size);
  // TODO: Publish to video/compressed topic using NNG
}

static void on_audio_frame_publish(uint8_t *data, int size) {
  // LOGD("Audio frame received, size: %d", size);
  // TODO: Publish to audio/compressed topic using NNG
}
