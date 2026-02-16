#include "nng/nng.h"
#include "nng/protocol/pubsub0/sub.h"
#include "peer.h"
#include "stdio.h"
#include "utils.h"
#include "video.h"
#include <cjson/cJSON.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
PeerConnection *g_subscriber_peer_connection = NULL;
PeerConnection *g_publisher_peer_connection = NULL;
pthread_t g_subscriber_thread;
pthread_t g_publisher_thread;
pthread_t g_data_handler_thread;
int g_terminate = 0;

static void onopen(void *user_data) {
  printf("on open\n");
  // Here you can send a message to the data channel if needed
  // Example: websocket_send_offer("Hello from onopen");
  // websocket_send_offer("Hello from onopen");
  // peer_connection_send_data(g_subscriber_peer_connection, "Hello from
  // onopen", strlen("Hello from onopen"));
}

static void onclose(void *user_data) { printf("on close\n"); }

static void onmessage(char *msg, size_t len, void *user_data, uint16_t sid) {
  printf("on message: %d %.*s", sid, (int)len, msg);
}

extern int websocket_send_offer(const char *offer);

int livekit_webrtc_subscriber_is_connected() {
  if (g_subscriber_peer_connection) {
    return peer_connection_get_state(g_subscriber_peer_connection) ==
           PEER_CONNECTION_CONNECTED;
  }
  return -1; // Not initialized
}

int livekit_webrtc_publisher_is_connected() {
  if (g_publisher_peer_connection) {
    return peer_connection_get_state(g_publisher_peer_connection);
  }
  return -1; // Not initialized
}

const char *livekit_webrtc_create_answer() {
  return peer_connection_create_answer(g_subscriber_peer_connection);
}

const char *livekit_webrtc_create_offer() {
  return peer_connection_create_offer(g_publisher_peer_connection);
}

void livekit_webrtc_set_remote_description(const char *sdp, const char *type) {
  if (strcmp(type, "offer") == 0) {
    LOGD("Setting remote SDP for subscriber peer: %s", sdp);
    peer_connection_set_remote_description(g_subscriber_peer_connection, sdp,
                                           SDP_TYPE_OFFER);
  } else if (strcmp(type, "answer") == 0) {
    LOGD("Setting remote SDP for publisher peer: %s", sdp);
    peer_connection_set_remote_description(g_publisher_peer_connection, sdp,
                                           SDP_TYPE_ANSWER);
  }
}

static void livekit_webrtc_add_ice_candidate(const char *candidate,
                                             PeerConnection *pc) {
  cJSON *json = cJSON_Parse(candidate);
  if (json == NULL) {
    return;
  }
  cJSON *candidate_json = cJSON_GetObjectItem(json, "candidate");
  if (candidate_json != NULL && cJSON_IsString(candidate_json)) {
    const char *candidate_str = candidate_json->valuestring;
    peer_connection_add_ice_candidate(pc, (char *)candidate_str);
  }
}

void livekit_webrtc_subscriber_add_ice_candidate(const char *candidate) {
  LOGD("Adding ICE candidate for subscriber: %s", candidate);
  livekit_webrtc_add_ice_candidate(candidate, g_subscriber_peer_connection);
}

void livekit_webrtc_publisher_add_ice_candidate(const char *candidate) {
  LOGD("Adding ICE candidate for publisher: %s", candidate);
  livekit_webrtc_add_ice_candidate(candidate, g_publisher_peer_connection);
}

static void livekit_webrtc_publisher_onstatechange(PeerConnectionState state,
                                                   void *userdata) {
  LOGI("Publisher peer connection state changed: %s",
       peer_connection_state_to_string(state));
}

static void livekit_webrtc_publisher_onicecandidate(char *description,
                                                    void *userdata) {}

static void livekit_webrtc_subscriber_onstatechange(PeerConnectionState state,
                                                    void *userdata) {
  LOGI("Subscriber peer connection state changed: %s",
       peer_connection_state_to_string(state));
}

static void livekit_webrtc_subscriber_onicecandidate(char *description,
                                                     void *userdata) {}

static void *livekit_webrtc_data_handler_thread(void *userdata) {
  nng_socket sock;
  // sub
  nng_sub0_open(&sock);
  nng_socket_set_string(sock, NNG_OPT_SUB_SUBSCRIBE, "");
  nng_dial(sock, TOPIC_VIDEO_COMPRESSED, NULL, NNG_FLAG_NONBLOCK);

  while (!g_terminate) {
    uint8_t *buf = NULL;
    size_t sz;
    int rv = nng_recv(sock, &buf, &sz, NNG_FLAG_ALLOC);
    if (rv == 0) {
      peer_connection_send_video(g_publisher_peer_connection, buf, sz);
      nng_free(buf, sz);
    }
    usleep(1000);
  }
}

static void livekit_webrtc_publisher_thread(void *userdata) {
  while (!g_terminate) {
    peer_connection_loop(g_publisher_peer_connection);
    usleep(1000);
  }
}

static void livekit_webrtc_subscriber_thread(void *userdata) {
  while (!g_terminate) {
    peer_connection_loop(g_subscriber_peer_connection);
    usleep(1000);
  }
}

static void onvideotrack(uint8_t *data, size_t size, void *userdata) {
  //  media_push_video(data, size);
}

static void onaudiotrack(uint8_t *data, size_t size, void *userdata) {
  // LOGD("Received audio track data of size: %zu", size);
}

void livekit_webrtc_send_video_data(uint8_t *data, size_t size) {
  if (g_publisher_peer_connection) {
    peer_connection_send_video(g_publisher_peer_connection, data, size);
  }
}

void livekit_webrtc_send_audio_data(uint8_t *data, size_t size) {
  if (g_publisher_peer_connection) {
    peer_connection_send_audio(g_publisher_peer_connection, data, size);
  }
}

void livekit_webrtc_create_peer_connections() {
  // media_start(
  //     livekit_webrtc_on_video_data,
  //     livekit_webrtc_on_audio_data
  //);
  PeerConfiguration publisher_config = {
      .ice_servers =
          {
              {.urls = "stun:stun.l.google.com:19302"},
          },
      .datachannel = DATA_CHANNEL_STRING,
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_H264,
      .onvideotrack = onvideotrack,
      .onaudiotrack = onaudiotrack};

  PeerConfiguration subscriber_config = {
      .ice_servers =
          {
              {.urls = "stun:stun.l.google.com:19302"},
          },
      .datachannel = DATA_CHANNEL_STRING,
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_H264,
      .onvideotrack = onvideotrack,
      .onaudiotrack = onaudiotrack};

  peer_init();

  g_subscriber_peer_connection = peer_connection_create(&subscriber_config);

  peer_connection_onicecandidate(g_subscriber_peer_connection,
                                 livekit_webrtc_subscriber_onicecandidate);

  peer_connection_oniceconnectionstatechange(
      g_subscriber_peer_connection, livekit_webrtc_subscriber_onstatechange);

  peer_connection_ondatachannel(g_subscriber_peer_connection, onmessage, onopen,
                                onclose);

  g_publisher_peer_connection = peer_connection_create(&publisher_config);

  peer_connection_onicecandidate(g_publisher_peer_connection,
                                 livekit_webrtc_publisher_onicecandidate);

  peer_connection_oniceconnectionstatechange(
      g_publisher_peer_connection, livekit_webrtc_publisher_onstatechange);

  peer_connection_ondatachannel(g_publisher_peer_connection, onmessage, onopen,
                                onclose);
  pthread_create(&g_subscriber_thread, NULL,
                 (void *)livekit_webrtc_subscriber_thread, NULL);

  pthread_create(&g_publisher_thread, NULL,
                 (void *)livekit_webrtc_publisher_thread, NULL);
  pthread_create(&g_data_handler_thread, NULL,
                 (void *)livekit_webrtc_data_handler_thread, NULL);
}

void livekit_webrtc_destroy_peer_connections() {
  g_terminate = 1;
  pthread_join(g_subscriber_thread, NULL);
  pthread_join(g_publisher_thread, NULL);

  if (g_subscriber_peer_connection) {
    peer_connection_destroy(g_subscriber_peer_connection);
    g_subscriber_peer_connection = NULL;
  }

  if (g_publisher_peer_connection) {
    peer_connection_destroy(g_publisher_peer_connection);
    g_publisher_peer_connection = NULL;
  }

  peer_deinit();
}
