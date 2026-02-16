#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>
#include "livekit.h"
#include "telegram.h"
#include "livekit.h"
#include "video.h"
#include "utils.h"
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

typedef int (*app_main_t)(void);

void* thread_adapter(void* arg) {
    app_main_t target_func = (app_main_t)arg;
    
    int result = target_func();
    printf("\n[系統] App 結束，回傳值為: %d\n", result);
    return (void*)(intptr_t)result;
}

void start_app(app_main_t func, const char* name) {
    pthread_t tid;
    printf("[系統] 正在啟動: %s\n", name);
    pthread_create(&tid, NULL, thread_adapter, (void*)func);
    pthread_detach(tid);
}

int main(int argc, char *argv[]) {
    nng_socket sock;
    int rv;
    start_app(app_telegram_main, "Telegram Bot");
    start_app(app_video_main, "video processing");
    if ((rv = nng_sub0_open(&sock)) != 0) {
        LOGE("nng_sub0_open: %s", nng_strerror(rv));
        return 1;
    }
    nng_socket_set(sock, NNG_OPT_SUB_SUBSCRIBE, "", 0);
    if ((rv = nng_dial(sock, TELEGRAM_UPDATES, NULL, NNG_FLAG_NONBLOCK)) != 0) {
        LOGE("nng_dial: %s", nng_strerror(rv));
        return 1;
    }
    LOGI("Subscribed to Telegram updates on %s", TELEGRAM_UPDATES);
    while (1) {
	char *buf = NULL;
        size_t sz;
        int rv = nng_recv(sock, &buf, &sz, NNG_FLAG_ALLOC);
        printf("Received message: %.*s\n", (int)sz, buf);
	if (strncmp(buf, "/meet", 5) == 0) {
          start_app(app_livekit_main, "LiveKit");
	}
	free(buf);
    }

    app_telegram_quit();
    nng_close(sock);
    return 0;
}
