#include "ini.h" // For inih library
#include "meet.h"
#include "telegram.h"
#include "utils.h"
#include "video.h"
#include "agent.h"
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
} AppConfig;

// Global instance of our application configuration
static AppConfig g_app_config = {
    .telegram_bot_token = NULL,
    .livekit_url = NULL,
    .livekit_token = NULL,
    .openai_api_key = NULL,
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
} ThreadArgs;

void *thread_adapter(void *args) {
  ThreadArgs *thread_args = (ThreadArgs *)args;
  int result = thread_args->func(thread_args->arg);
  printf("\n[系統] App 結束，回傳值為: %d\n", result);
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

  nng_socket sock;
  int rv;

  // Start Telegram Bot
  start_app((app_main_func_t)app_telegram_main, "Telegram Bot",
            (void *)g_app_config.telegram_bot_token);

  // Start video processing (assuming it doesn't need config directly from ini
  // for now)
  start_app((app_main_func_t)app_video_main, "video processing", NULL);
  start_app((app_main_func_t)app_agent_main, "Agent", (void *)g_app_config.openai_api_key);


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
  nng_close(sock);

  // Free allocated config strings
  free(g_app_config.telegram_bot_token);
  free(g_app_config.livekit_url);
  free(g_app_config.livekit_token);
  free(g_app_config.openai_api_key);

  return 0;
}
