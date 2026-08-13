#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int fatimg_open(const char* path, uint64_t size_bytes);
void fatimg_close(void);
#ifdef __cplusplus
}
#endif
