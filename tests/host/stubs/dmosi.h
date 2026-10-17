#ifndef TEST_DMOSI_H
#define TEST_DMOSI_H
#include <stdbool.h>
#include <stdint.h>
typedef struct test_mutex *dmosi_mutex_t;
dmosi_mutex_t dmosi_mutex_create(bool recursive);
void dmosi_mutex_destroy(dmosi_mutex_t mutex);
int dmosi_mutex_lock(dmosi_mutex_t mutex);
int dmosi_mutex_unlock(dmosi_mutex_t mutex);
uint32_t dmosi_get_tick_count(void);
#endif
