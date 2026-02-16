#include "telegram.h"
#include "utils.h"
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <stdbool.h> // For bool type
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile bool g_telegram_running = true;
static nng_socket
    g_nng_sock; // Global NNG socket to allow closing from app_telegram_quit

#define TIMEOUT 10

struct buffer {
  char *data;
  size_t size;
};

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t total = size * nmemb;
  struct buffer *buf = userdata;

  char *tmp = realloc(buf->data, buf->size + total + 1);
  if (!tmp)
    return 0;

  buf->data = tmp;
  memcpy(buf->data + buf->size, ptr, total);
  buf->size += total;
  buf->data[buf->size] = '\0';

  return total;
}

void send_message(CURL *curl, long long chat_id, const char *text,
                  const char *bot_token) {
  char url[256];
  char post[512];

  char *escaped = curl_easy_escape(curl, text, 0);

  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage",
           bot_token);

  snprintf(post, sizeof(post), "chat_id=%lld&text=%s", chat_id, escaped);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_perform(curl);
  curl_free(escaped);
}

int app_telegram_main(void *arg) {
  const char *bot_token = (const char *)arg;
  CURL *curl;
  long last_update_id = 0;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();
  if (!curl) {
    LOGE("curl init failed");
    return 1;
  }

  int rv;

  nng_pub0_open(&g_nng_sock);
  nng_listen(g_nng_sock, TOPIC_TELEGRAM_UPDATES, NULL, 0);

  while (g_telegram_running) {
    char url[512];
    struct buffer buf = {0};

    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/getUpdates?timeout=%d&offset=%ld",
             bot_token, TIMEOUT, last_update_id + 1);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, TIMEOUT + 5);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    if (curl_easy_perform(curl) != CURLE_OK) {
      free(buf.data);
      continue;
    }

    do {
      cJSON *root = cJSON_Parse(buf.data);
      if (!root) {
        break;
      }

      cJSON *result = cJSON_GetObjectItem(root, "result");
      if (!cJSON_IsArray(result)) {
        break;
      }

      // process last update
      int update_count = cJSON_GetArraySize(result);
      if (update_count == 0) {
        break;
      }
      LOGI("received: %s", buf.data);
      cJSON *last_update = cJSON_GetArrayItem(result, update_count - 1);
      if (!last_update || !cJSON_IsObject(last_update)) {
        break;
      }

      cJSON *update_id = cJSON_GetObjectItem(last_update, "update_id");
      if (!update_id || !cJSON_IsNumber(update_id)) {
        break;
      }
      last_update_id = update_id->valuedouble;

      cJSON *msg = cJSON_GetObjectItem(last_update, "message");
      if (!msg || !cJSON_IsObject(msg)) {
        break;
      }

      cJSON *chat = cJSON_GetObjectItem(msg, "chat");
      if (!chat || !cJSON_IsObject(chat)) {
        break;
      }
      cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
      if (!chat_id || !cJSON_IsNumber(chat_id)) {
        break;
      }
      int64_t chat_id_value = chat_id->valuedouble;
      cJSON *text = cJSON_GetObjectItem(msg, "text");
      if (!text || !cJSON_IsString(text)) {
        break;
      }
      LOGI("cmd: %s", text->valuestring);
      nng_send(g_nng_sock, text->valuestring, strlen(text->valuestring), 0);
      cJSON_Delete(root);
      send_message(curl, chat_id_value, "Message received!", bot_token);
    } while (0);

    free(buf.data);
  }

  nng_close(g_nng_sock); // Close the NNG socket when the loop terminates
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  return 0;
}

void app_telegram_quit(void) {
  LOGI("Attempting to quit Telegram app.");
  g_telegram_running = false;
  // Close the NNG socket to unblock app_telegram_main if it's waiting
  // on nng_listen or nng_recvmsg (though nng_listen might not block in this
  // pub/sub setup, future blocking calls might). For now, it's global and
  // closed in main. If app_telegram_main is in a separate thread, this would
  // signal it to exit.
}
