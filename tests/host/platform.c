#define _POSIX_C_SOURCE 200809L
#include "dmod.h"
#include "dmosi.h"
#include "dmutc.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
static _Atomic uint32_t ticks_offset;
static _Atomic int64_t utc_offset;
static _Atomic bool utc_available = true;
static _Atomic size_t allocations;
void test_ticks_advance(uint32_t ms)
{
    ticks_offset += ms;
}
void test_utc_offset(int64_t seconds)
{
    utc_offset = seconds;
}
void test_utc_available(int available)
{
    utc_available = available != 0;
}
size_t test_live_allocations(void)
{
    return allocations;
}
void *Dmod_Malloc(size_t size)
{
    void *p = malloc(size);
    if (p != NULL)
        ++allocations;
    return p;
}
void Dmod_Free(void *p)
{
    if (p != NULL)
        --allocations;
    free(p);
}
int Dmod_VSnPrintf(char *out, size_t n, const char *fmt, va_list args)
{
    return vsnprintf(out, n, fmt, args);
}
struct test_mutex
{
    pthread_mutex_t native;
};
dmosi_mutex_t dmosi_mutex_create(bool recursive)
{
    dmosi_mutex_t p = malloc(sizeof(*p));
    if (p == NULL)
        return NULL;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (recursive)
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int rc = pthread_mutex_init(&p->native, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0)
    {
        free(p);
        return NULL;
    }
    return p;
}
void dmosi_mutex_destroy(dmosi_mutex_t p)
{
    pthread_mutex_destroy(&p->native);
    free(p);
}
int dmosi_mutex_lock(dmosi_mutex_t p)
{
    return pthread_mutex_lock(&p->native);
}
int dmosi_mutex_unlock(dmosi_mutex_t p)
{
    return pthread_mutex_unlock(&p->native);
}
uint32_t dmosi_get_tick_count(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000) + ticks_offset;
}
int dmutc_get(dmutc_time_t *out)
{
    if (!utc_available)
        return -ENODATA;
    *out = (dmutc_time_t){time(NULL) + utc_offset, 0, 0};
    return 0;
}
int dmutc_to_calendar(const dmutc_time_t *value, dmutc_calendar_t *out)
{
    time_t t = (time_t)value->seconds;
    struct tm c;
    if (gmtime_r(&t, &c) == NULL)
        return -ERANGE;
    *out = (dmutc_calendar_t){
        c.tm_year + 1900, c.tm_mon + 1, c.tm_mday, c.tm_hour, c.tm_min, c.tm_sec, 0, 0, 0};
    return 0;
}
int Dmod_SnPrintf(char *out, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int rc = vsnprintf(out, n, fmt, args);
    va_end(args);
    return rc;
}
