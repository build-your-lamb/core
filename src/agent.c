#include "agent.h"
#include "peer.h"
#include "utils.h"
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

int g_interrupted = 0;
PeerConnection *g_pc = NULL;
PeerConnectionState g_state;

#define OPENAI_API_URL "https://api.openai.com/v1/realtime/calls"

struct MemoryStruct {
  char *memory;
  size_t size;
};

// libcurl 回調函數
static size_t WriteMemoryCallback(void *contents, size_t size, size_t nmemb,
                                  void *userp) {
  size_t realsize = size * nmemb;
  struct MemoryStruct *mem = (struct MemoryStruct *)userp;
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if (!ptr)
    return 0;
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
  return realsize;
}

/**
 * 獲取 OpenAI Realtime Token
 * 回傳：成功則回傳 token 字串 (需手動 free)，失敗回傳 NULL
 */
char *get_openai_realtime_token(const char *api_key) {
  CURL *curl;
  CURLcode res;
  struct MemoryStruct chunk = {malloc(1), 0};
  char *token = NULL;

  const char *url = "https://api.openai.com/v1/realtime/client_secrets";
  const char *payload = "{\"session\": {\"type\": \"realtime\", \"model\": "
                        "\"gpt-4o-realtime-preview-2024-10-01\", \"audio\": "
                        "{\"output\": {\"voice\": \"marin\"}}}}";

  curl = curl_easy_init();
  if (!curl)
    return NULL;

  struct curl_slist *headers = NULL;
  char auth_header[256];
  snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
           api_key);

  headers = curl_slist_append(headers, auth_header);
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

  res = curl_easy_perform(curl);

  if (res == CURLE_OK) {
    LOGI("Response from OpenAI API: %s",
         chunk.memory); // Debug print for the raw response
    cJSON *json = cJSON_Parse(chunk.memory);
    if (json) {
      cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "value");
      if (cJSON_IsString(value) && (value->valuestring != NULL)) {
        token = strdup(value->valuestring); // 複製一份 token 用於回傳
      }
      cJSON_Delete(json);
    }
  }

  // 清理資源
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  free(chunk.memory);

  return token;
}

static void onconnectionstatechange(PeerConnectionState state, void *data) {
  printf("state is changed: %s\n", peer_connection_state_to_string(state));
  g_state = state;
}

static void onopen(void *user_data) {}

static void onclose(void *user_data) {}

static void onmessage(char *msg, size_t len, void *user_data, uint16_t sid) {
  printf("on message: %d %.*s", sid, (int)len, msg);
}

static void *peer_singaling_task(void *data) {
  while (!g_interrupted) {
    peer_signaling_loop();
    usleep(1000);
  }

  pthread_exit(NULL);
}

static void *peer_connection_task(void *data) {
  while (!g_interrupted) {
    peer_connection_loop(g_pc);
    usleep(1000);
  }

  pthread_exit(NULL);
}

static uint64_t get_timestamp() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int app_agent_main(void *args) {
  char *openai_api_key = (char *)args;
  char *openai_realtime_token = NULL;
  openai_realtime_token = get_openai_realtime_token(openai_api_key);
  if (openai_realtime_token == NULL) {
    return 1;
  }

  // Debug print for the obtained token
  LOGI("OpenAI Realtime Token obtained: %s", openai_realtime_token);

  pthread_t peer_singaling_thread;
  pthread_t peer_connection_thread;

  PeerConfiguration config = {.ice_servers =
                                  {
                                      {.urls = "stun:stun.l.google.com:19302"},
                                  },
                              .datachannel = DATA_CHANNEL_STRING,
                              .audio_codec = CODEC_OPUS};

  peer_init();
  LOGI("Peer initialized");
  g_pc = peer_connection_create(&config);
  peer_connection_oniceconnectionstatechange(g_pc, onconnectionstatechange);
  peer_connection_ondatachannel(g_pc, onmessage, onopen, onclose);

  pthread_create(&peer_connection_thread, NULL, peer_connection_task, NULL);
  pthread_create(&peer_singaling_thread, NULL, peer_singaling_task, NULL);

  peer_signaling_connect(OPENAI_API_URL, openai_realtime_token, g_pc);

  while (!g_interrupted) {
    usleep(1000);
  }

  pthread_join(peer_singaling_thread, NULL);
  pthread_join(peer_connection_thread, NULL);

  peer_signaling_disconnect();
  peer_connection_destroy(g_pc);
  peer_deinit();

  // Free allocated resources (OpenAI token, and AgentArgs members + struct)
  free(openai_realtime_token);
  return 0;
}

void app_agent_quit() {}
