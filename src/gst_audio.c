#include "audio.h"
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

static const char MIC_PIPELINE[] =
    "audiotestsrc ! opusenc ! appsink name=sink";
static const char SPK_PIPELINE[] =
    "appsrc name=src format=time ! opusparse ! opusdec ! alsasink";

static GstElement *g_mic_pipeline = NULL;
static GstElement *g_mic_sink = NULL;
static GstElement *g_spk_pipeline = NULL;
static GstElement *g_spk_src = NULL;
static nng_socket g_nng_audio_pub_sock = { .id = -1 };
static nng_socket g_nng_audio_sub_sock = { .id = -1 };
static volatile bool g_running = false;

static GstFlowReturn on_audio_data(GstElement *sink, void *data) {
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
      rv = nng_sendmsg(g_nng_audio_pub_sock, msg, 0);
      if (rv != 0) {
        LOGE("gst_audio: nng_sendmsg error: %s", nng_strerror(rv));
        nng_msg_free(msg);
      }
    } else {
      LOGE("gst_audio: nng_msg_alloc error: %s", nng_strerror(rv));
    }
  }

  gst_buffer_unmap(buffer, &info);
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

int app_audio_main(void *arg) {
  (void)arg;

  int err;

  gst_init(NULL, NULL);

  if ((err = nng_pub0_open(&g_nng_audio_pub_sock)) != 0) {
    LOGE("app_audio_main: nng_pub0_open error: %s", nng_strerror(err));
    return 1;
  }

  if ((err = nng_listen(g_nng_audio_pub_sock, TOPIC_AUDIO_COMPRESSED, NULL,
                        0)) != 0) {
    LOGE("app_audio_main: nng_listen error: %s", nng_strerror(err));
    nng_close(g_nng_audio_pub_sock);
    g_nng_audio_pub_sock.id = -1;
    return 1;
  }

  g_mic_pipeline = gst_parse_launch(MIC_PIPELINE, NULL);
  if (!g_mic_pipeline) {
    LOGE("app_audio_main: failed to create mic pipeline");
    app_audio_quit();
    return 1;
  }

  g_spk_pipeline = gst_parse_launch(SPK_PIPELINE, NULL);
  if (!g_spk_pipeline) {
    LOGE("app_audio_main: failed to create spk pipeline");
    app_audio_quit();
    return 1;
  }

  g_mic_sink = gst_bin_get_by_name(GST_BIN(g_mic_pipeline), "sink");
  if (!g_mic_sink) {
    LOGE("app_audio_main: failed to get mic appsink");
    app_audio_quit();
    return 1;
  }

  g_spk_src = gst_bin_get_by_name(GST_BIN(g_spk_pipeline), "src");
  if (!g_spk_src) {
    LOGE("app_audio_main: failed to get spk appsrc");
    app_audio_quit();
    return 1;
  }

  g_signal_connect(g_mic_sink, "new-sample", G_CALLBACK(on_audio_data), NULL);
  g_object_set(g_mic_sink, "emit-signals", TRUE, NULL);
  g_object_set(g_spk_src, "emit-signals", TRUE, NULL);

  gst_element_set_state(g_mic_pipeline, GST_STATE_PLAYING);
  gst_element_set_state(g_spk_pipeline, GST_STATE_PLAYING);

  if ((err = nng_sub0_open(&g_nng_audio_sub_sock)) != 0) {
    LOGE("app_audio_main: nng_sub0_open error: %s", nng_strerror(err));
    app_audio_quit();
    return 1;
  }
  nng_socket_set_string(g_nng_audio_sub_sock, NNG_OPT_SUB_SUBSCRIBE, "");
  if ((err = nng_dial(g_nng_audio_sub_sock, TOPIC_AUDIO_WEBRTC, NULL,
                      NNG_FLAG_NONBLOCK)) != 0) {
    LOGE("app_audio_main: nng_dial error: %s", nng_strerror(err));
    app_audio_quit();
    return 1;
  }

  g_running = true;
  while (g_running) {
    uint8_t *buf = NULL;
    size_t sz = 0;
    int rv = nng_recv(g_nng_audio_sub_sock, &buf, &sz,
                      NNG_FLAG_ALLOC | NNG_FLAG_NONBLOCK);
    if (rv == 0 && buf && sz > 0) {
      GstBuffer *gst_buf = gst_buffer_new_and_alloc(sz);
      GstMapInfo info;
      if (gst_buffer_map(gst_buf, &info, GST_MAP_WRITE)) {
        memcpy(info.data, buf, sz);
        info.size = sz;
        GstFlowReturn *ret = NULL;
        g_signal_emit_by_name(g_spk_src, "push-buffer", gst_buf, &ret);
        gst_buffer_unmap(gst_buf, &info);
      }
      gst_buffer_unref(gst_buf);
      nng_free(buf, sz);
    }
    usleep(1000);
  }

  app_audio_quit();
  return 0;
}

void app_audio_quit(void) {
  g_running = false;

  if (g_mic_pipeline) {
    gst_element_set_state(g_mic_pipeline, GST_STATE_NULL);
  }
  if (g_spk_pipeline) {
    gst_element_set_state(g_spk_pipeline, GST_STATE_NULL);
  }

  if (g_mic_sink) {
    g_object_unref(g_mic_sink);
    g_mic_sink = NULL;
  }
  if (g_spk_src) {
    g_object_unref(g_spk_src);
    g_spk_src = NULL;
  }

  if (g_mic_pipeline) {
    g_object_unref(g_mic_pipeline);
    g_mic_pipeline = NULL;
  }
  if (g_spk_pipeline) {
    g_object_unref(g_spk_pipeline);
    g_spk_pipeline = NULL;
  }

  if (nng_socket_id(g_nng_audio_sub_sock) != -1) {
    nng_close(g_nng_audio_sub_sock);
    g_nng_audio_sub_sock.id = -1;
  }
  if (nng_socket_id(g_nng_audio_pub_sock) != -1) {
    nng_close(g_nng_audio_pub_sock);
    g_nng_audio_pub_sock.id = -1;
  }
}
