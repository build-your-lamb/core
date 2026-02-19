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
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include "video.h"
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
#include <rk_aiq_user_api2_camgroup.h>
#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_sysctl.h>

pthread_t main_thread;
void (*media_on_video_frame)(uint8_t *data, int size) = NULL;
void (*media_on_audio_frame)(uint8_t *data, int size) = NULL;
static int media_quit_flag = 0;
static pthread_mutex_t media_mutex = PTHREAD_MUTEX_INITIALIZER;
MPP_CHN_S stSrcChn, stDestChn;

static void *GetMediaBuffer0(void *arg) {
}


static RK_S32 g_s32FrameCnt = -1;
static bool quit = false;

#define MAX_AIQ_CTX 8
static rk_aiq_sys_ctx_t *g_aiq_ctx[MAX_AIQ_CTX];
rk_aiq_working_mode_t g_WDRMode[MAX_AIQ_CTX];
#include <stdatomic.h>
static atomic_int g_sof_cnt = 0;
static atomic_bool g_should_quit = false;

static XCamReturn SIMPLE_COMM_ISP_SofCb(rk_aiq_metas_t *meta) {
	g_sof_cnt++;
	if (g_sof_cnt <= 2)
		printf("=== %u ===\n", meta->frame_id);
	return XCAM_RETURN_NO_ERROR;
}

static XCamReturn SIMPLE_COMM_ISP_ErrCb(rk_aiq_err_msg_t *msg) {
	if (msg->err_code == XCAM_RETURN_BYPASS)
		g_should_quit = true;
	return XCAM_RETURN_NO_ERROR;
}

RK_S32 SIMPLE_COMM_ISP_Init(RK_S32 CamId, rk_aiq_working_mode_t WDRMode, RK_BOOL MultiCam,
                            const char *iq_file_dir) {
	if (CamId >= MAX_AIQ_CTX) {
		printf("%s : CamId is over 3\n", __FUNCTION__);
		return -1;
	}
	// char *iq_file_dir = "iqfiles/";
	setlinebuf(stdout);
	if (iq_file_dir == NULL) {
		printf("SIMPLE_COMM_ISP_Init : not start.\n");
		g_aiq_ctx[CamId] = NULL;
		return 0;
	}

	// must set HDR_MODE, before init
	g_WDRMode[CamId] = WDRMode;
	char hdr_str[16];
	snprintf(hdr_str, sizeof(hdr_str), "%d", (int)WDRMode);
	setenv("HDR_MODE", hdr_str, 1);

	rk_aiq_sys_ctx_t *aiq_ctx;
	rk_aiq_static_info_t aiq_static_info;
#ifdef RV1126_RV1109
	rk_aiq_uapi_sysctl_enumStaticMetas(CamId, &aiq_static_info);

	printf("ID: %d, sensor_name is %s, iqfiles is %s\n", CamId,
	       aiq_static_info.sensor_info.sensor_name, iq_file_dir);

	aiq_ctx =
	    rk_aiq_uapi_sysctl_init(aiq_static_info.sensor_info.sensor_name, iq_file_dir,
	                             SIMPLE_COMM_ISP_ErrCb, SIMPLE_COMM_ISP_SofCb);

	if (MultiCam)
		rk_aiq_uapi_sysctl_setMulCamConc(aiq_ctx, true);
#else
	rk_aiq_uapi2_sysctl_enumStaticMetas(CamId, &aiq_static_info);

	printf("ID: %d, sensor_name is %s, iqfiles is %s\n", CamId,
	       aiq_static_info.sensor_info.sensor_name, iq_file_dir);

	aiq_ctx =
	    rk_aiq_uapi2_sysctl_init(aiq_static_info.sensor_info.sensor_name, iq_file_dir,
	                             SIMPLE_COMM_ISP_ErrCb, SIMPLE_COMM_ISP_SofCb);

	if (MultiCam)
		rk_aiq_uapi2_sysctl_setMulCamConc(aiq_ctx, true);
#endif

	g_aiq_ctx[CamId] = aiq_ctx;
	return 0;
}


RK_S32 SIMPLE_COMM_ISP_Run(RK_S32 CamId) {
	if (CamId >= MAX_AIQ_CTX || !g_aiq_ctx[CamId]) {
		printf("%s : CamId is over 3 or not init\n", __FUNCTION__);
		return -1;
	}
#ifdef RV1126_RV1109
	if (rk_aiq_uapi_sysctl_prepare(g_aiq_ctx[CamId], 0, 0, g_WDRMode[CamId])) {
		printf("rkaiq engine prepare failed !\n");
		g_aiq_ctx[CamId] = NULL;
		return -1;
	}
	printf("rk_aiq_uapi_sysctl_init/prepare succeed\n");
	if (rk_aiq_uapi_sysctl_start(g_aiq_ctx[CamId])) {
		printf("rk_aiq_uapi_sysctl_start  failed\n");
		return -1;
	}
	printf("rk_aiq_uapi_sysctl_start succeed\n");
#else
	if (rk_aiq_uapi2_sysctl_prepare(g_aiq_ctx[CamId], 0, 0, g_WDRMode[CamId])) {
		printf("rkaiq engine prepare failed !\n");
		g_aiq_ctx[CamId] = NULL;
		return -1;
	}
	printf("rk_aiq_uapi2_sysctl_init/prepare succeed\n");
	if (rk_aiq_uapi2_sysctl_start(g_aiq_ctx[CamId])) {
		printf("rk_aiq_uapi2_sysctl_start  failed\n");
		return -1;
	}
	printf("rk_aiq_uapi2_sysctl_start succeed\n");
#endif
	return 0;
}

RK_S32 SIMPLE_COMM_ISP_Stop(RK_S32 CamId) {
	if (CamId >= MAX_AIQ_CTX || !g_aiq_ctx[CamId]) {
		printf("%s : CamId is over 3 or not init g_aiq_ctx[%d] = %p\n", __FUNCTION__,
		       CamId, g_aiq_ctx[CamId]);
		return -1;
	}
#ifdef RV1126_RV1109
	printf("rk_aiq_uapi_sysctl_stop enter\n");
	rk_aiq_uapi_sysctl_stop(g_aiq_ctx[CamId], false);
	printf("rk_aiq_uapi_sysctl_deinit enter\n");
	rk_aiq_uapi_sysctl_deinit(g_aiq_ctx[CamId]);
	printf("rk_aiq_uapi_sysctl_deinit exit\n");
#else
	printf("rk_aiq_uapi2_sysctl_stop enter\n");
	rk_aiq_uapi2_sysctl_stop(g_aiq_ctx[CamId], false);
	printf("rk_aiq_uapi2_sysctl_deinit enter\n");
	rk_aiq_uapi2_sysctl_deinit(g_aiq_ctx[CamId]);
	printf("rk_aiq_uapi2_sysctl_deinit exit\n");
#endif

	g_aiq_ctx[CamId] = NULL;
	return 0;
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

int app_video_main(void) {
  RK_MPI_SYS_Init();
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

		SIMPLE_COMM_ISP_Init(0, RK_AIQ_WORKING_MODE_NORMAL, 0, "/etc/iqfiles/");
		SIMPLE_COMM_ISP_Run(0);

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

  void *pData = RK_NULL;

  VENC_STREAM_S stFrame;
  stFrame.pstPack = malloc(sizeof(VENC_PACK_S));

  nng_socket sock;
  nng_pub0_open(&sock);
  // listen on 
  nng_listen(sock, TOPIC_VIDEO_COMPRESSED, NULL, 0);

  while (!media_quit_flag) {
    s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
    if (s32Ret == RK_SUCCESS) {
      pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
      // fwrite(pData, 1, stFrame.pstPack->u32Len, venc0_file);
      //LOGI("get stream success, size:%d", stFrame.pstPack->u32Len);
      // publish to nng
      nng_send(sock, pData, stFrame.pstPack->u32Len, NNG_FLAG_NONBLOCK);
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
  s32Ret = RK_MPI_SYS_UnBind(&stSrcChn, &stDestChn);
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
 RK_MPI_SYS_Exit(); 
  SIMPLE_COMM_ISP_Stop(0);
}

void app_video_quit(void) {
  media_quit_flag = 1;
}
