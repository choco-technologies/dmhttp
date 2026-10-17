#ifndef TEST_DMUTC_H
#define TEST_DMUTC_H
#include <stdint.h>
typedef struct
{
    int64_t seconds;
    uint32_t nanosecond, reserved;
} dmutc_time_t;
typedef struct
{
    int32_t year;
    uint8_t month, day, hour, minute, second, weekday;
    uint16_t reserved;
    uint32_t nanosecond;
} dmutc_calendar_t;
int dmutc_get(dmutc_time_t *);
int dmutc_to_calendar(const dmutc_time_t *, dmutc_calendar_t *);
#endif
