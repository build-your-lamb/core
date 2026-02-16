#include "utils.h"
#include <stdlib.h>

int utils_queue_init(utils_queue_t *q, size_t capacity) {
  q->buf = (void **)malloc(sizeof(void *) * capacity);
  if (!q->buf)
    return -1;
  q->capacity = capacity;
  q->head = 0;
  q->tail = 0;
  pthread_mutex_init(&q->mtx, NULL);
  return 0;
}

void utils_queue_destroy(utils_queue_t *q) {
  free(q->buf);
  pthread_mutex_destroy(&q->mtx);
}

int utils_queue_push(utils_queue_t *q, void *item) {
  pthread_mutex_lock(&q->mtx);
  size_t next = (q->tail + 1) % q->capacity;
  if (next == q->head) {
    pthread_mutex_unlock(&q->mtx);
    return -1; // full
  }
  q->buf[q->tail] = item;
  q->tail = next;
  pthread_mutex_unlock(&q->mtx);
  return 0;
}

int utils_queue_pop(utils_queue_t *q, void **item) {
  pthread_mutex_lock(&q->mtx);
  if (q->head == q->tail) {
    pthread_mutex_unlock(&q->mtx);
    return -1; // empty
  }
  *item = q->buf[q->head];
  q->head = (q->head + 1) % q->capacity;
  pthread_mutex_unlock(&q->mtx);
  return 0;
}
