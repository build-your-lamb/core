#include "meet.h" // Renamed from livekit.h
#include "peer.h" // From webrtc.c
#include "utils.h"
#include "utlist.h"
#include "video.h"       // From webrtc.c
#include <cjson/cJSON.h> // From webrtc.c
#include <libwebsockets.h>
#include <livekit_rtc.pb-c.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h> // Also from webrtc.c
#include <pthread.h>                  // From webrtc.c
#include <stdio.h>                    // From webrtc.c
#include <string.h>
#include <unistd.h> // From webrtc.c

#define kVideoBus "inproc://video_bus"
#define kAudioBus "inproc://audio_bus" // New audio bus

typedef struct WriteableBuffer {
  uint8_t *data;
  int size;
  struct WriteableBuffer *next;
} WriteableBuffer;

typedef struct PerSessionData {
  struct lws *wsi;
} PerSessionData;

int g_video_track_published_ = 0;
WriteableBuffer *g_wb_queue_ = NULL;
uint8_t g_received_buffer_[8192];
int g_received_offset_ = 0;

static nng_socket g_video_nng_sub_sock_;
static pthread_t g_video_subscriber_tid_;
static bool g_video_subscriber_running_ = false;

static nng_socket g_audio_nng_sub_sock_; // New NNG socket for audio
static pthread_t g_audio_subscriber_tid_;
static bool g_audio_subscriber_running_ = false;

static const char *
ResponseMessageToString(Livekit__SignalResponse__MessageCase message_case) {
  switch (message_case) {
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE__NOT_SET:
    return "NOT_SET (Ping/Pong)";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_JOIN:
    return "JOIN";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ANSWER:
    return "ANSWER";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_OFFER:
    return "OFFER";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRICKLE:
    return "TRICKLE";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_UPDATE:
    return "UPDATE";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRACK_PUBLISHED:
    return "TRACK_PUBLISHED";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_LEAVE:
    return "LEAVE";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_MUTE:
    return "MUTE";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_SPEAKERS_CHANGED:
    return "SPEAKERS_CHANGED";
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ROOM_UPDATE:
    return "ROOM_UPDATE";
  default:
    return "UNKNOWN";
  }
}

static void MeetRequestAddAudioTrack(struct lws *wsi) {
  Livekit__SignalRequest r = LIVEKIT__SIGNAL_REQUEST__INIT;
  Livekit__AddTrackRequest a = LIVEKIT__ADD_TRACK_REQUEST__INIT;

  a.cid = (char *)"microphone";
  a.name = (char *)"microphone";
  a.source = LIVEKIT__TRACK_SOURCE__MICROPHONE;

  r.add_track = &a;
  r.message_case = LIVEKIT__SIGNAL_REQUEST__MESSAGE_ADD_TRACK;

  size_t size = livekit__signal_request__get_packed_size(&r);

  WriteableBuffer *wb = (WriteableBuffer *)calloc(1, sizeof(WriteableBuffer));
  wb->data = (uint8_t *)malloc(size + LWS_PRE);
  wb->size = size;
  livekit__signal_request__pack(&r, wb->data + LWS_PRE);
  LL_APPEND(g_wb_queue_, wb);
  lws_callback_on_writable(wsi);
}

static void MeetRequestAddVideoTrack(struct lws *wsi) {
  Livekit__SignalRequest r = LIVEKIT__SIGNAL_REQUEST__INIT;
  Livekit__AddTrackRequest a = LIVEKIT__ADD_TRACK_REQUEST__INIT;

  a.cid = (char *)"camera";
  a.name = (char *)"camera";
  a.type = LIVEKIT__TRACK_TYPE__VIDEO;
  a.source = LIVEKIT__TRACK_SOURCE__CAMERA;

  r.add_track = &a;
  r.message_case = LIVEKIT__SIGNAL_REQUEST__MESSAGE_ADD_TRACK;

  size_t size = livekit__signal_request__get_packed_size(&r);

  WriteableBuffer *wb = (WriteableBuffer *)calloc(1, sizeof(WriteableBuffer));
  wb->data = (uint8_t *)malloc(size + LWS_PRE);
  wb->size = size;
  livekit__signal_request__pack(&r, wb->data + LWS_PRE);
  LL_APPEND(g_wb_queue_, wb);
  lws_callback_on_writable(wsi);
}

static void MeetRequestAnswer(struct lws *wsi, const char *sdp) {
  Livekit__SignalRequest r = LIVEKIT__SIGNAL_REQUEST__INIT;
  Livekit__SessionDescription s = LIVEKIT__SESSION_DESCRIPTION__INIT;
  s.sdp = (char *)sdp;
  s.type = "answer";
  r.answer = &s;
  r.message_case = LIVEKIT__SIGNAL_REQUEST__MESSAGE_ANSWER;

  size_t size = livekit__signal_request__get_packed_size(&r);
  WriteableBuffer *wb = (WriteableBuffer *)calloc(1, sizeof(WriteableBuffer));
  wb->data = (uint8_t *)malloc(size + LWS_PRE);
  wb->size = size;
  livekit__signal_request__pack(&r, wb->data + LWS_PRE);
  LL_APPEND(g_wb_queue_, wb);
  lws_callback_on_writable(wsi);
}

static void MeetRequestOffer(struct lws *wsi, const char *sdp) {
  Livekit__SignalRequest r = LIVEKIT__SIGNAL_REQUEST__INIT;
  Livekit__SessionDescription s = LIVEKIT__SESSION_DESCRIPTION__INIT;
  s.sdp = (char *)sdp;
  s.type = "offer";
  r.offer = &s;
  r.message_case = LIVEKIT__SIGNAL_REQUEST__MESSAGE_OFFER;
  size_t size = livekit__signal_request__get_packed_size(&r);
  WriteableBuffer *wb = (WriteableBuffer *)calloc(1, sizeof(WriteableBuffer));
  wb->data = (uint8_t *)malloc(size + LWS_PRE);
  wb->size = size;
  livekit__signal_request__pack(&r, wb->data + LWS_PRE);
  LL_APPEND(g_wb_queue_, wb);
  lws_callback_on_writable(wsi);
}

static void MeetHandleResponse(Livekit__SignalResponse *response,
                               struct lws *wsi) {
  assert(response != NULL);
  const char *msg_case = ResponseMessageToString(response->message_case);
  switch (response->message_case) {
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_JOIN:
    LOGI("Join message received\n");
    MeetRequestAddAudioTrack(wsi);
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ANSWER:
    LOGI("Answer message received %s", response->answer->sdp);
    MeetWebrtcSetRemoteDescription(response->answer->sdp, "answer");
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_OFFER: {
    LOGI("Offer message received %s", response->offer->sdp);
    MeetWebrtcSetRemoteDescription(response->offer->sdp, "offer");
    const char *answer = MeetWebrtcCreateAnswer();
    LOGI("Creating answer: %s", answer);
    MeetRequestAnswer(wsi, answer);
  } break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRICKLE:
    if (strstr(response->trickle->candidateinit, "tcp") != NULL) {
      LOGW("TCP candidate received, ignoring");
      break;
    }
    LOGI("Trickle message received %d:%s\n", response->trickle->target,
         response->trickle->candidateinit);
    if (response->trickle->target == LIVEKIT__SIGNAL_TARGET__SUBSCRIBER) {
      MeetWebrtcSubscriberAddIceCandidate(response->trickle->candidateinit);
    } else if (response->trickle->target == LIVEKIT__SIGNAL_TARGET__PUBLISHER) {
      MeetWebrtcPublisherAddIceCandidate(response->trickle->candidateinit);
    }
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_UPDATE:
    LOGI("Update message received\n");
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRACK_PUBLISHED:
    LOGI("Track published message received\n");
    {
      if (!g_video_track_published_) {
        MeetRequestAddVideoTrack(wsi);
        g_video_track_published_ = 1;
      } else {
        const char *offer = MeetWebrtcCreateOffer();
        MeetRequestOffer(wsi, offer);
      }
    }
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_LEAVE:
    LOGW("Leave reason: %d, action: %d, %d", response->leave->reason,
         response->leave->action, response->leave->can_reconnect);
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_MUTE:
    LOGI("Mute message received\n");
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_SPEAKERS_CHANGED:
    LOGI("Speakers changed message received\n");
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ROOM_UPDATE:
    LOGI("Room update message received\n");
    break;
  default:
    LOGI("Unknown message type %d", response->message_case);
  }
}

static int MeetCallback(struct lws *wsi, enum lws_callback_reasons reason,
                        void *user, void *in, size_t len) {
  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    LOGI("Connection established");
    break;
  case LWS_CALLBACK_CLIENT_RECEIVE: {
    memcpy(g_received_buffer_ + g_received_offset_, in, len);
    g_received_offset_ += len;
    if (lws_is_final_fragment(wsi)) {
      Livekit__SignalResponse *response = livekit__signal_response__unpack(
          NULL, g_received_offset_, g_received_buffer_);
      MeetHandleResponse(response, wsi);
      g_received_offset_ = 0; // Reset offset after processing
    }
  } break;
  case LWS_CALLBACK_CLIENT_WRITEABLE: {

    if (g_wb_queue_ != NULL) {
      WriteableBuffer *wb = g_wb_queue_;
      LL_DELETE(g_wb_queue_, wb);
      lws_write(wsi, wb->data + LWS_PRE, wb->size, LWS_WRITE_BINARY);
      free(wb->data);
      free(wb);
    }

    if (g_wb_queue_ != NULL) {
      lws_callback_on_writable(wsi);
    }
  } break;
  case LWS_CALLBACK_CLOSED:
    LOGI("Connection closed");
    break;
  default:
    break;
  }
  return 0;
}

static struct lws_protocols protocols[] = {
    {"ws", MeetCallback, sizeof(struct PerSessionData), 0}, {NULL, NULL, 0, 0}};

int MeetConnect(const char *url, const char *token) {
  struct lws_context_creation_info info = {0};
  info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.port = CONTEXT_PORT_NO_LISTEN;
  info.protocols = protocols;

  struct lws_context *context = lws_create_context(&info);
  char path[4096];
  memset(path, 0, sizeof(path));
  snprintf(path, sizeof(path),
           "/rtc?protocol=3&access_token=%s&auto_subscribe=true", token);
  LOGI("path: %s\n", path);
  struct lws_client_connect_info ccinfo = {0};
  ccinfo.context = context;
  ccinfo.address = url;
  ccinfo.port = 443;
  ccinfo.path = path;
  ccinfo.host = ccinfo.address;
  ccinfo.origin = "origin";
  ccinfo.protocol = protocols[0].name;
  ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED;
  lws_client_connect_via_info(&ccinfo);

  MeetWebrtcCreatePeerConnections();

  //  nng_socket video_recv_sock;
  //  nng_sub0_open(&video_recv_sock);
  //  nng_socket_set_string(video_recv_sock, NNG_OPT_SUB_SUBSCRIBE, "");
  // int nng_dial(nng_socket s, const char *url, nng_dialer *dp, int flags);
  //  nng_dial(video_recv_sock, "inproc://video.compressed", NULL,
  //  NNG_FLAG_NONBLOCK);

  while (1) {
    lws_service(context, 1);
    // sub
    uint8_t *video_msg;
#if 0
    size_t video_msg_len;
    int rv = nng_recv(video_recv_sock, &video_msg, &video_msg_len, NNG_FLAG_ALLOC);
    if (rv == 0) {
      // Process video_msg
//      MeetWebrtcSendVideoData(video_msg, video_msg_len);
      LOGI("Received video message of length %zu", video_msg_len);
      nng_free(video_msg, video_msg_len);
    }
#endif
  }

  lws_context_destroy(context);
  g_video_subscriber_running_ = false;
  pthread_join(g_video_subscriber_tid_, NULL);
  g_audio_subscriber_running_ = false;
  pthread_join(g_audio_subscriber_tid_, NULL);
  return 0;
}

int AppMeetMain(void *arg) {
  MeetArgs *meet_args = (MeetArgs *)arg;
  if (!meet_args) {
    LOGE("MeetArgs are NULL");
    return 1;
  }
  int ret = MeetConnect(meet_args->url, meet_args->token);
  free(meet_args); // Free the allocated MeetArgs
  return ret;
}

void AppMeetQuit() { MeetWebrtcDestroyPeerConnections(); }

// Content from webrtc.c starts here
PeerConnection *g_subscriber_peer_connection_ = NULL;
PeerConnection *g_publisher_peer_connection_ = NULL;
pthread_t g_subscriber_thread_;
pthread_t g_publisher_thread_;
pthread_t g_data_handler_thread_;
int g_terminate_ = 0;

static void OnOpen(void *user_data) {
  printf("on open\n");
  // Here you can send a message to the data channel if needed
  // Example: WebsocketSendOffer("Hello from onopen");
  // WebsocketSendOffer("Hello from onopen");
  // peer_connection_send_data(g_subscriber_peer_connection_, "Hello from
  // onopen", strlen("Hello from onopen"));
}

static void OnClose(void *user_data) { printf("on close\n"); }

static void OnMessage(char *msg, size_t len, void *user_data, uint16_t sid) {
  printf("on message: %d %.*s", sid, (int)len, msg);
}

extern int WebsocketSendOffer(const char *offer);

int MeetWebrtcSubscriberIsConnected() {
  if (g_subscriber_peer_connection_) {
    return peer_connection_get_state(g_subscriber_peer_connection_) ==
           PEER_CONNECTION_CONNECTED;
  }
  return -1; // Not initialized
}

int MeetWebrtcPublisherIsConnected() {
  if (g_publisher_peer_connection_) {
    return peer_connection_get_state(g_publisher_peer_connection_);
  }
  return -1; // Not initialized
}

const char *MeetWebrtcCreateAnswer() {
  return peer_connection_create_answer(g_subscriber_peer_connection_);
}

const char *MeetWebrtcCreateOffer() {
  return peer_connection_create_offer(g_publisher_peer_connection_);
}

void MeetWebrtcSetRemoteDescription(const char *sdp, const char *type) {
  if (strcmp(type, "offer") == 0) {
    LOGD("Setting remote SDP for subscriber peer: %s", sdp);
    peer_connection_set_remote_description(g_subscriber_peer_connection_, sdp,
                                           SDP_TYPE_OFFER);
  } else if (strcmp(type, "answer") == 0) {
    LOGD("Setting remote SDP for publisher peer: %s", sdp);
    peer_connection_set_remote_description(g_publisher_peer_connection_, sdp,
                                           SDP_TYPE_ANSWER);
  }
}

static void MeetWebrtcAddIceCandidate(const char *candidate,
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

void MeetWebrtcSubscriberAddIceCandidate(const char *candidate) {
  LOGD("Adding ICE candidate for subscriber: %s", candidate);
  MeetWebrtcAddIceCandidate(candidate, g_subscriber_peer_connection_);
}

void MeetWebrtcPublisherAddIceCandidate(const char *candidate) {
  LOGD("Adding ICE candidate for publisher: %s", candidate);
  MeetWebrtcAddIceCandidate(candidate, g_publisher_peer_connection_);
}

static void MeetWebrtcPublisherOnStateChange(PeerConnectionState state,
                                             void *userdata) {
  LOGI("Publisher peer connection state changed: %s",
       peer_connection_state_to_string(state));
}

static void MeetWebrtcPublisherOnIceCandidate(char *description,
                                              void *userdata) {}

static void MeetWebrtcSubscriberOnStateChange(PeerConnectionState state,
                                              void *userdata) {
  LOGI("Subscriber peer connection state changed: %s",
       peer_connection_state_to_string(state));
}

static void MeetWebrtcSubscriberOnIceCandidate(char *description,
                                               void *userdata) {}

static void *MeetWebrtcDataHandlerThread(void *userdata) {
  nng_socket sock;
  // sub
  nng_sub0_open(&sock);
  nng_socket_set_string(sock, NNG_OPT_SUB_SUBSCRIBE, "");
  nng_dial(sock, TOPIC_VIDEO_COMPRESSED, NULL, NNG_FLAG_NONBLOCK);

  while (!g_terminate_) {
    uint8_t *buf = NULL;
    size_t sz;
    int rv = nng_recv(sock, &buf, &sz, NNG_FLAG_ALLOC);
    if (rv == 0) {
      peer_connection_send_video(g_publisher_peer_connection_, buf, sz);
      nng_free(buf, sz);
    }
    usleep(1000);
  }
  return NULL;
}

static void MeetWebrtcPublisherThread(void *userdata) {
  while (!g_terminate_) {
    peer_connection_loop(g_publisher_peer_connection_);
    usleep(1000);
  }
}

static void MeetWebrtcSubscriberThread(void *userdata) {
  while (!g_terminate_) {
    peer_connection_loop(g_subscriber_peer_connection_);
    usleep(1000);
  }
}

static void OnVideoTrack(uint8_t *data, size_t size, void *userdata) {
  //  media_push_video(data, size);
}

static void OnAudioTrack(uint8_t *data, size_t size, void *userdata) {
  // LOGD("Received audio track data of size: %zu", size);
}

void MeetWebrtcSendVideoData(uint8_t *data, size_t size) {
  if (g_publisher_peer_connection_) {
    peer_connection_send_video(g_publisher_peer_connection_, data, size);
  }
}

void MeetWebrtcSendAudioData(uint8_t *data, size_t size) {
  if (g_publisher_peer_connection_) {
    peer_connection_send_audio(g_publisher_peer_connection_, data, size);
  }
}

void MeetWebrtcCreatePeerConnections() {
  // media_start(
  //     MeetWebrtcOnVideoData,
  //     MeetWebrtcOnAudioData
  //);
  PeerConfiguration publisher_config = {
      .ice_servers =
          {
              {.urls = "stun:stun.l.google.com:19302"},
          },
      .datachannel = DATA_CHANNEL_STRING,
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_H264,
      .onvideotrack = OnVideoTrack,
      .onaudiotrack = OnAudioTrack};

  PeerConfiguration subscriber_config = {
      .ice_servers =
          {
              {.urls = "stun:stun.l.google.com:19302"},
          },
      .datachannel = DATA_CHANNEL_STRING,
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_H264,
      .onvideotrack = OnVideoTrack,
      .onaudiotrack = OnAudioTrack};

  peer_init();

  g_subscriber_peer_connection_ = peer_connection_create(&subscriber_config);

  peer_connection_onicecandidate(g_subscriber_peer_connection_,
                                 MeetWebrtcSubscriberOnIceCandidate);

  peer_connection_oniceconnectionstatechange(g_subscriber_peer_connection_,
                                             MeetWebrtcSubscriberOnStateChange);

  peer_connection_ondatachannel(g_subscriber_peer_connection_, OnMessage,
                                OnOpen, OnClose);

  g_publisher_peer_connection_ = peer_connection_create(&publisher_config);

  peer_connection_onicecandidate(g_publisher_peer_connection_,
                                 MeetWebrtcPublisherOnIceCandidate);

  peer_connection_oniceconnectionstatechange(g_publisher_peer_connection_,
                                             MeetWebrtcPublisherOnStateChange);

  peer_connection_ondatachannel(g_publisher_peer_connection_, OnMessage, OnOpen,
                                OnClose);
  pthread_create(&g_subscriber_thread_, NULL,
                 (void *)MeetWebrtcSubscriberThread, NULL);

  pthread_create(&g_publisher_thread_, NULL, (void *)MeetWebrtcPublisherThread,
                 NULL);
  pthread_create(&g_data_handler_thread_, NULL,
                 (void *)MeetWebrtcDataHandlerThread, NULL);
}

void MeetWebrtcDestroyPeerConnections() {
  g_terminate_ = 1;
  pthread_join(g_subscriber_thread_, NULL);
  pthread_join(g_publisher_thread_, NULL);
  pthread_join(g_data_handler_thread_, NULL); // Join data handler thread

  if (g_subscriber_peer_connection_) {
    peer_connection_destroy(g_subscriber_peer_connection_);
    g_subscriber_peer_connection_ = NULL;
  }

  if (g_publisher_peer_connection_) {
    peer_connection_destroy(g_publisher_peer_connection_);
    g_publisher_peer_connection_ = NULL;
  }

  peer_deinit();
}
