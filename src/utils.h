#ifndef UTILS_H_
#define UTILS_H_

#include <stddef.h>
#include <pthread.h>

/*
 * utils_queue - utility queue library
 * generic pointer queue, non-blocking
 * part of utils library
 */
typedef struct {
    void **buf;          // array of void* (generic)
    size_t capacity;     // max number of elements
    size_t head;         // pop index
    size_t tail;         // push index
    pthread_mutex_t mtx; // protect head/tail
} utils_queue_t;

/**
 * Initialize the queue
 * capacity: max number of items
 * returns 0 if success, -1 if memory allocation failed
 */
int utils_queue_init(utils_queue_t *q, size_t capacity);

/**
 * Destroy the queue
 * Does NOT free item memory
 */
void utils_queue_destroy(utils_queue_t *q);

/**
 * Push item into queue (non-blocking)
 * Returns 0 if success, -1 if full
 */
int utils_queue_push(utils_queue_t *q, void *item);

/**
 * Pop item from queue (non-blocking)
 * Returns 0 if success, -1 if empty
 */
int utils_queue_pop(utils_queue_t *q, void **item);

#define LEVEL_ERROR 0x00
#define LEVEL_WARN 0x01
#define LEVEL_INFO 0x02
#define LEVEL_DEBUG 0x03

#define ERROR_TAG "ERROR"
#define WARN_TAG "WARN"
#define INFO_TAG "INFO"
#define DEBUG_TAG "DEBUG"

#define LOG_LEVEL LEVEL_DEBUG

#define LOG_PRINT(level_tag, fmt, ...) \
  fprintf(stdout, "%s\t%s\t%d\t" fmt "\n", level_tag, __FILE__, __LINE__, ##__VA_ARGS__)

#if LOG_LEVEL >= LEVEL_DEBUG
#define LOGD(fmt, ...) LOG_PRINT(DEBUG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGD(fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_INFO
#define LOGI(fmt, ...) LOG_PRINT(INFO_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGI(fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_WARN
#define LOGW(fmt, ...) LOG_PRINT(WARN_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGW(fmt, ...)
#endif

#if LOG_LEVEL >= LEVEL_ERROR
#define LOGE(fmt, ...) LOG_PRINT(ERROR_TAG, fmt, ##__VA_ARGS__)
#else
#define LOGE(fmt, ...)
#endif

#endif // UTILS_QUEUE_H_

