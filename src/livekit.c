#include "livekit.h"
#include "utils.h"
#include "utlist.h"
#include "webrtc.h"
#include <libwebsockets.h>
#include <livekit_rtc.pb-c.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>
#include <string.h>

#define VIDEO_BUS "inproc://video_bus"
#define AUDIO_BUS "inproc://audio_bus" // New audio bus

typedef struct WriteableBuffer {
  uint8_t *data;
  int size;
  struct WriteableBuffer *next;
} WriteableBuffer;

typedef struct PerSessionData {
  struct lws *wsi;
} PerSessionData;

int video_track_published = 0;
WriteableBuffer *wb_queue = NULL;
uint8_t received_buffer[8192];
int received_offset = 0;

static nng_socket video_nng_sub_sock;
static pthread_t video_subscriber_tid;
static bool video_subscriber_running = false;

static nng_socket audio_nng_sub_sock; // New NNG socket for audio
static pthread_t audio_subscriber_tid;
static bool audio_subscriber_running = false;

static const char *
response_message_to_string(Livekit__SignalResponse__MessageCase message_case) {
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

static void livekit_request_add_audiotrack(struct lws *wsi) {
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
  LL_APPEND(wb_queue, wb);
  lws_callback_on_writable(wsi);
}

static void livekit_request_add_videotrack(struct lws *wsi) {
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
  LL_APPEND(wb_queue, wb);
  lws_callback_on_writable(wsi);
}

static void livekit_request_answer(struct lws *wsi, const char *sdp) {
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
  LL_APPEND(wb_queue, wb);
  lws_callback_on_writable(wsi);
}

static void livekit_request_offer(struct lws *wsi, const char *sdp) {
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
  LL_APPEND(wb_queue, wb);
  lws_callback_on_writable(wsi);
}

static void livekit_handle_response(Livekit__SignalResponse *response,
                                    struct lws *wsi) {
  assert(response != NULL);
  const char *msg_case = response_message_to_string(response->message_case);
  switch (response->message_case) {
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_JOIN:
    LOGI("Join message received\n");
    livekit_request_add_audiotrack(wsi);
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_ANSWER:
    LOGI("Answer message received %s", response->answer->sdp);
    livekit_webrtc_set_remote_description(response->answer->sdp, "answer");
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_OFFER: {
    LOGI("Offer message received %s", response->offer->sdp);
    livekit_webrtc_set_remote_description(response->offer->sdp, "offer");
    const char *answer = livekit_webrtc_create_answer();
    LOGI("Creating answer: %s", answer);
    livekit_request_answer(wsi, answer);
  } break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRICKLE:
    if (strstr(response->trickle->candidateinit, "tcp") != NULL) {
      LOGW("TCP candidate received, ignoring");
      break;
    }
    LOGI("Trickle message received %d:%s\n", response->trickle->target,
         response->trickle->candidateinit);
    if (response->trickle->target == LIVEKIT__SIGNAL_TARGET__SUBSCRIBER) {
      livekit_webrtc_subscriber_add_ice_candidate(
          response->trickle->candidateinit);
    } else if (response->trickle->target == LIVEKIT__SIGNAL_TARGET__PUBLISHER) {
      livekit_webrtc_publisher_add_ice_candidate(
          response->trickle->candidateinit);
    }
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_UPDATE:
    LOGI("Update message received\n");
    break;
  case LIVEKIT__SIGNAL_RESPONSE__MESSAGE_TRACK_PUBLISHED:
    LOGI("Track published message received\n");
    {
      if (!video_track_published) {
        livekit_request_add_videotrack(wsi);
        video_track_published = 1;
      } else {
        const char *offer = livekit_webrtc_create_offer();
        livekit_request_offer(wsi, offer);
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

static int livekit_callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
  switch (reason) {
  case LWS_CALLBACK_CLIENT_ESTABLISHED:
    LOGI("Connection established");
    break;
  case LWS_CALLBACK_CLIENT_RECEIVE: {
    memcpy(received_buffer + received_offset, in, len);
    received_offset += len;
    if (lws_is_final_fragment(wsi)) {
      Livekit__SignalResponse *response = livekit__signal_response__unpack(
          NULL, received_offset, received_buffer);
      livekit_handle_response(response, wsi);
      received_offset = 0; // Reset offset after processing
    }
  } break;
  case LWS_CALLBACK_CLIENT_WRITEABLE: {

    if (wb_queue != NULL) {
      WriteableBuffer *wb = wb_queue;
      LL_DELETE(wb_queue, wb);
      lws_write(wsi, wb->data + LWS_PRE, wb->size, LWS_WRITE_BINARY);
      free(wb->data);
      free(wb);
    }

    if (wb_queue != NULL) {
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
    {"ws", livekit_callback, sizeof(struct PerSessionData), 0},
    {NULL, NULL, 0, 0}};

int livekit_connect(const char *url, const char *token) {
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

  livekit_webrtc_create_peer_connections();

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
//      livekit_webrtc_send_video_data(video_msg, video_msg_len);
      LOGI("Received video message of length %zu", video_msg_len);
      nng_free(video_msg, video_msg_len);
    }
#endif
  }

  lws_context_destroy(context);
  video_subscriber_running = false;
  pthread_join(video_subscriber_tid, NULL);
  audio_subscriber_running = false;
  pthread_join(audio_subscriber_tid, NULL);
  return 0;
}

int app_livekit_main(void *arg) {
  LiveKitArgs *lk_args = (LiveKitArgs *)arg;
  if (!lk_args) {
    LOGE("LiveKitArgs are NULL");
    return 1;
  }
  int ret = livekit_connect(lk_args->url, lk_args->token);
  free(lk_args); // Free the allocated LiveKitArgs
  return ret;
}

void app_livekit_quit() {}
