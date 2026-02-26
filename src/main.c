#include "agent.h"
#include "audio.h"
#include "display.h"
#include "ini.h" // For inih library
#include "meet.h"
#include "telegram.h"
#include "utils.h"
#include "video.h"
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Structure to hold our application configuration
typedef struct {
  char *telegram_bot_token;
  char *livekit_url;
  char *livekit_token;
  char *openai_api_key;
  char *video_cam_pipeline;
  char *video_dis_pipeline;
  char *audio_mic_pipeline;
  char *audio_spk_pipeline;
} AppConfig;

// Global instance of our application configuration
static AppConfig g_app_config = {
    .telegram_bot_token = NULL,
    .livekit_url = NULL,
    .livekit_token = NULL,
    .openai_api_key = NULL,
    .video_cam_pipeline = NULL,
    .video_dis_pipeline = NULL,
    .audio_mic_pipeline = NULL,
    .audio_spk_pipeline = NULL,
};

// Handler function for inih
static int config_ini_handler(void *user, const char *section, const char *name,
                              const char *value) {
  AppConfig *pconfig = (AppConfig *)user;

#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0

  if (MATCH("telegram", "bot_token")) {
    pconfig->telegram_bot_token = strdup(value);
  } else if (MATCH("livekit", "url")) {
    pconfig->livekit_url = strdup(value);
  } else if (MATCH("livekit", "token")) {

    pconfig->livekit_token = strdup(value);
    LOGI("Loaded LiveKit token: %s", pconfig->livekit_token);
  } else if (MATCH("openai", "api_key")) {
    pconfig->openai_api_key = strdup(value);
    LOGI("Loaded OpenAI API Key");
  } else if (MATCH("video", "cam")) {
    pconfig->video_cam_pipeline = strdup(value);
  } else if (MATCH("video", "dis")) {
    pconfig->video_dis_pipeline = strdup(value);
  } else if (MATCH("audio", "mic")) {
    pconfig->audio_mic_pipeline = strdup(value);
  } else if (MATCH("audio", "spk")) {
    pconfig->audio_spk_pipeline = strdup(value);
  } else {
    return 0; // Unknown section/name, error
  }
  return 1;
}

typedef int (*app_main_func_t)(void *);

// Structure to pass function pointer and its argument to the thread
typedef struct {
  app_main_func_t func;
  void *arg;
  const char *name;
  int retry_limit;
  int retry_delay_ms;
} ThreadArgs;

void *thread_adapter(void *args) {
  ThreadArgs *thread_args = (ThreadArgs *)args;
  int attempt = 0;
  int result = 0;
  while (1) {
    result = thread_args->func(thread_args->arg);
    printf("\n[系統] App 結束 (%s)，回傳值為: %d\n", thread_args->name,
           result);
    if (result == 0 || attempt >= thread_args->retry_limit) {
      break;
    }
    attempt++;
    printf("[系統] App %s 異常結束，準備重啟 (%d/%d)\n", thread_args->name,
           attempt, thread_args->retry_limit);
    usleep(thread_args->retry_delay_ms * 1000);
  }
  free(thread_args); // Free the dynamically allocated ThreadArgs
  return (void *)(intptr_t)result;
}

void start_app(app_main_func_t func, const char *name, void *arg) {
  pthread_t tid;
  ThreadArgs *thread_args = (ThreadArgs *)malloc(sizeof(ThreadArgs));
  if (thread_args == NULL) {
    LOGE("Failed to allocate ThreadArgs");
    return;
  }
  thread_args->func = func;
  thread_args->arg = arg;
  thread_args->name = name;
  thread_args->retry_limit = 1;
  thread_args->retry_delay_ms = 500;

  printf("[系統] 正在啟動: %s\n", name);
  pthread_create(&tid, NULL, thread_adapter, (void *)thread_args);
  pthread_detach(tid);
}

int main(int argc, char *argv[]) {
  // Load configuration
  if (ini_parse("lamb.ini", config_ini_handler, &g_app_config) < 0) {
    LOGE("Can't load 'lamb.ini'");
    return 1;
  }

  // Validate configuration
  if (!g_app_config.telegram_bot_token) {
    LOGE("Telegram bot token not found in 'lamb.ini'. Please provide it under "
         "[telegram] bot_token=...");
    return 1;
  }
  if (!g_app_config.livekit_url) {
    LOGE("LiveKit URL not found in 'lamb.ini'. Please provide it under "
         "[livekit] url=...");
    return 1;
  }
  if (!g_app_config.livekit_token) {
    LOGE("LiveKit Token not found in 'lamb.ini'. Please provide it under "
         "[livekit] token=...");
    return 1;
  }
  if (!g_app_config.openai_api_key) {
    LOGE("OpenAI API Key not found in 'lamb.ini'. Please provide it under "
         "[openai] api_key=...");
    return 1;
  }
  LOGI("Configuration loaded successfully from 'lamb.ini'");

  app_video_set_pipelines(g_app_config.video_cam_pipeline,
                          g_app_config.video_dis_pipeline);
  app_audio_set_pipelines(g_app_config.audio_mic_pipeline,
                          g_app_config.audio_spk_pipeline);



  nng_socket sock;
  int rv;

  // Start Telegram Bot
  start_app((app_main_func_t)app_telegram_main, "Telegram Bot",
            (void *)g_app_config.telegram_bot_token);

  start_app((app_main_func_t)app_video_main, "Video", NULL);
//  start_app((app_main_func_t)app_agent_main, "Agent",
//            (void *)g_app_config.openai_api_key);
  start_app((app_main_func_t)app_audio_main, "Audio", NULL);
  start_app((app_main_func_t)app_display_main, "Display", NULL);

  if ((rv = nng_sub0_open(&sock)) != 0) {
    LOGE("nng_sub0_open: %s", nng_strerror(rv));
    return 1;
  }
  nng_socket_set(sock, NNG_OPT_SUB_SUBSCRIBE, "", 0);
  if ((rv = nng_dial(sock, TOPIC_TELEGRAM_UPDATES, NULL, NNG_FLAG_NONBLOCK)) !=
      0) {
    LOGE("nng_dial: %s", nng_strerror(rv));
    return 1;
  }
  while (1) {
    char *buf = NULL;
    size_t sz;
    int rv = nng_recv(sock, &buf, &sz, NNG_FLAG_ALLOC);
    printf("Received message: %.*s\n", (int)sz, buf);
    if (buf && strncmp(buf, "/meet", 5) == 0) {
      MeetArgs meet_args = {
          .url = g_app_config.livekit_url,
          .token = g_app_config.livekit_token,
      };
      start_app((app_main_func_t)AppMeetMain, "LiveKit", (void *)&meet_args);
    }
    free(buf);
  }

  app_telegram_quit();
  AppMeetQuit();
  app_audio_quit();
  nng_close(sock);

  // Free allocated config strings
  free(g_app_config.telegram_bot_token);
  free(g_app_config.livekit_url);
  free(g_app_config.livekit_token);
  free(g_app_config.openai_api_key);
  free(g_app_config.video_cam_pipeline);
  free(g_app_config.video_dis_pipeline);
  free(g_app_config.audio_mic_pipeline);
  free(g_app_config.audio_spk_pipeline);
  return 0;
}
