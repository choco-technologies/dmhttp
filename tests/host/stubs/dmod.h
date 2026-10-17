#ifndef TEST_DMOD_H
#define TEST_DMOD_H
#define DMOD_GLOBAL_POINTER __attribute__((section(".got")))
#define DMOD_MOD_DEFS_H_dmutc
#define dmod_dmutc_api(v, r, n, a) r dmutc##n a
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
typedef struct
{
    int unused;
} Dmod_Config_t;
void *Dmod_Malloc(size_t size);
void Dmod_Free(void *p);
int Dmod_VSnPrintf(char *, size_t, const char *, va_list);
#define dmod_dmtls_api_declaration(v, r, n, a) r dmtls##n a
#endif
#define dmod_dmhttp_api_declaration(v, r, n, a) r dmhttp##n a
int Dmod_SnPrintf(char *, size_t, const char *, ...);
