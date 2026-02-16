#ifndef MEET_H_
#define MEET_H_
#include <stdint.h> // For uint8_t
#include <stdlib.h>

// Structure to pass Meet specific arguments
typedef struct {
  const char *url;
  const char *token;
} MeetArgs;

#define kLivekitVideoWidth 640
#define kLivekitVideoHeight 480

int AppMeetMain(void *arg);
void AppMeetQuit();

// Declarations from webrtc.h
void MeetWebrtcCreatePeerConnections();
void MeetWebrtcDestroyPeerConnections();
const char *MeetWebrtcCreateAnswer();
const char *MeetWebrtcCreateOffer();
int MeetWebrtcSubscriberIsConnected();
int MeetWebrtcPublisherIsConnected();
void MeetWebrtcSetRemoteDescription(const char *sdp, const char *type);
void MeetWebrtcSubscriberAddIceCandidate(const char *candidate);
void MeetWebrtcPublisherAddIceCandidate(const char *candidate);
void MeetWebrtcSendVideoData(uint8_t *frame_data, size_t size);
void MeetWebrtcSendAudioData(uint8_t *frame_data, size_t size);

#endif // MEET_H_
