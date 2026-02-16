#ifndef WEBRTC_H_
#define WEBRTC_H_

#include <string.h>

void livekit_webrtc_create_peer_connections();
void livekit_webrtc_destroy_peer_connections();
const char* livekit_webrtc_create_answer();
const char* livekit_webrtc_create_offer();
int livekit_webrtc_subscriber_is_connected();
int livekit_webrtc_publisher_is_connected();
void livekit_webrtc_set_remote_description(const char* sdp, const char* type);
void livekit_webrtc_subscriber_add_ice_candidate(const char* candidate);
void livekit_webrtc_publisher_add_ice_candidate(const char* candidate);
void livekit_webrtc_send_video_data(uint8_t* frame_data, int size);
void livekit_webrtc_send_audio_data(uint8_t* frame_data, int size);

#endif  // WEBRTC_H_
