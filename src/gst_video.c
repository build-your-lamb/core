#include "video.h"
#include "utils.h"

#include <gst/gst.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char DEFAULT_CAM_PIPELINE[] =
    "libcamerasrc ! video/x-raw,width=1280,height=720,format=NV12 ! v4l2convert "
    "! v4l2h264enc extra-controls=\"controls,repeat_sequence_header=1\" "
    "! video/x-h264,level=(string)4 ! appsink name=sink";
static const char DEFAULT_DIS_PIPELINE[] =
    "appsrc name=src is-live=true do-timestamp=true format=time "
    "! queue ! h264parse ! v4l2h264dec qos=false output-io-mode=4 "
    "capture-io-mode=4 ! glimagesink";

static char *g_cam_pipeline_desc = NULL;
static char *g_dis_pipeline_desc = NULL;

static GstElement *g_cam_pipeline = NULL;
static GstElement *g_cam_sink = NULL;
static GstElement *g_dis_pipeline = NULL;
static GstElement *g_dis_src = NULL;
static nng_socket g_nng_video_pub_sock = { .id = -1 };
static nng_socket g_nng_video_sub_sock = { .id = -1 };
static volatile bool g_running = false;

static const char *get_cam_pipeline_desc(void) {
  return g_cam_pipeline_desc ? g_cam_pipeline_desc : DEFAULT_CAM_PIPELINE;
}

static const char *get_dis_pipeline_desc(void) {
  return g_dis_pipeline_desc ? g_dis_pipeline_desc : DEFAULT_DIS_PIPELINE;
}

void app_video_set_pipelines(const char *cam_pipeline,
                             const char *dis_pipeline) {
  if (cam_pipeline && cam_pipeline[0] != '\0') {
    free(g_cam_pipeline_desc);
    g_cam_pipeline_desc = strdup(cam_pipeline);
  }
  if (dis_pipeline && dis_pipeline[0] != '\0') {
    free(g_dis_pipeline_desc);
    g_dis_pipeline_desc = strdup(dis_pipeline);
  }
}

static GstFlowReturn on_video_data(GstElement *sink, void *data) {
  (void)data;

  GstSample *sample = NULL;
  GstBuffer *buffer = NULL;
  GstMapInfo info;

  g_signal_emit_by_name(sink, "pull-sample", &sample);
  if (!sample) {
    return GST_FLOW_ERROR;
  }

  buffer = gst_sample_get_buffer(sample);
  if (!buffer) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  if (!gst_buffer_map(buffer, &info, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return GST_FLOW_ERROR;
  }

  if (info.size > 0) {
    nng_msg *msg = NULL;
    int rv = nng_msg_alloc(&msg, info.size);
    if (rv == 0) {
      memcpy(nng_msg_body(msg), info.data, info.size);
      rv = nng_sendmsg(g_nng_video_pub_sock, msg, 0);
      if (rv != 0) {
        LOGE("gst_video: nng_sendmsg error: %s", nng_strerror(rv));
        nng_msg_free(msg);
      }
    } else {
      LOGE("gst_video: nng_msg_alloc error: %s", nng_strerror(rv));
    }
  }

  gst_buffer_unmap(buffer, &info);
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

int app_video_main(void *arg) {
  (void)arg;

  int err;

  gst_init(NULL, NULL);

  if ((err = nng_pub0_open(&g_nng_video_pub_sock)) != 0) {
    LOGE("app_video_main: nng_pub0_open error: %s", nng_strerror(err));
    return 1;
  }

  if ((err = nng_listen(g_nng_video_pub_sock, TOPIC_VIDEO_COMPRESSED, NULL,
                        0)) != 0) {
    LOGE("app_video_main: nng_listen error: %s", nng_strerror(err));
    nng_close(g_nng_video_pub_sock);
    g_nng_video_pub_sock.id = -1;
    return 1;
  }

  g_cam_pipeline = gst_parse_launch(get_cam_pipeline_desc(), NULL);
  if (!g_cam_pipeline) {
    LOGE("app_video_main: failed to create cam pipeline");
    app_video_quit();
    return 1;
  }

  g_dis_pipeline = gst_parse_launch(get_dis_pipeline_desc(), NULL);
  if (!g_dis_pipeline) {
    LOGE("app_video_main: failed to create display pipeline");
    app_video_quit();
    return 1;
  }

  g_cam_sink = gst_bin_get_by_name(GST_BIN(g_cam_pipeline), "sink");
  if (!g_cam_sink) {
    LOGE("app_video_main: failed to get cam appsink");
    app_video_quit();
    return 1;
  }

  g_dis_src = gst_bin_get_by_name(GST_BIN(g_dis_pipeline), "src");
  if (!g_dis_src) {
    LOGE("app_video_main: failed to get display appsrc");
    app_video_quit();
    return 1;
  }

  g_signal_connect(g_cam_sink, "new-sample", G_CALLBACK(on_video_data), NULL);
  g_object_set(g_cam_sink, "emit-signals", TRUE, NULL);
  g_object_set(g_dis_src, "emit-signals", TRUE, "is-live", TRUE,
               "do-timestamp", TRUE, "block", FALSE, NULL);

  gst_element_set_state(g_cam_pipeline, GST_STATE_PLAYING);
  gst_element_set_state(g_dis_pipeline, GST_STATE_PLAYING);

  if ((err = nng_sub0_open(&g_nng_video_sub_sock)) != 0) {
    LOGE("app_video_main: nng_sub0_open error: %s", nng_strerror(err));
    app_video_quit();
    return 1;
  }
  nng_socket_set_string(g_nng_video_sub_sock, NNG_OPT_SUB_SUBSCRIBE, "");
  if ((err = nng_dial(g_nng_video_sub_sock, TOPIC_VIDEO_WEBRTC, NULL,
                      NNG_FLAG_NONBLOCK)) != 0) {
    LOGE("app_video_main: nng_dial error: %s", nng_strerror(err));
    app_video_quit();
    return 1;
  }

  g_running = true;
  while (g_running) {
    uint8_t *buf = NULL;
    size_t sz = 0;
    int rv = nng_recv(g_nng_video_sub_sock, &buf, &sz,
                      NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
    if (rv == 0 && buf && sz > 0) {
      GstBuffer *gst_buf = gst_buffer_new_and_alloc(sz);
      GstMapInfo info;
      if (gst_buffer_map(gst_buf, &info, GST_MAP_WRITE)) {
        memcpy(info.data, buf, sz);
        info.size = sz;
        GstFlowReturn *ret = NULL;
        g_signal_emit_by_name(g_dis_src, "push-buffer", gst_buf, &ret);
        gst_buffer_unmap(gst_buf, &info);
      }
      gst_buffer_unref(gst_buf);
      nng_free(buf, sz);
    }
    usleep(1000);
  }

  app_video_quit();
  return 0;
}

void app_video_quit(void) {
  g_running = false;

  if (g_cam_pipeline) {
    gst_element_set_state(g_cam_pipeline, GST_STATE_NULL);
  }
  if (g_dis_pipeline) {
    gst_element_set_state(g_dis_pipeline, GST_STATE_NULL);
  }

  if (g_cam_sink) {
    g_object_unref(g_cam_sink);
    g_cam_sink = NULL;
  }
  if (g_dis_src) {
    g_object_unref(g_dis_src);
    g_dis_src = NULL;
  }

  if (g_cam_pipeline) {
    g_object_unref(g_cam_pipeline);
    g_cam_pipeline = NULL;
  }
  if (g_dis_pipeline) {
    g_object_unref(g_dis_pipeline);
    g_dis_pipeline = NULL;
  }

  free(g_cam_pipeline_desc);
  g_cam_pipeline_desc = NULL;
  free(g_dis_pipeline_desc);
  g_dis_pipeline_desc = NULL;

  if (nng_socket_id(g_nng_video_sub_sock) != -1) {
    nng_close(g_nng_video_sub_sock);
    g_nng_video_sub_sock.id = -1;
  }
  if (nng_socket_id(g_nng_video_pub_sock) != -1) {
    nng_close(g_nng_video_pub_sock);
    g_nng_video_pub_sock.id = -1;
  }
}
