/* Minimal NVM3 stand-in for the host test. Only what od_cfg_index.c calls. */
#ifndef STUB_NVM3_DEFAULT_H
#define STUB_NVM3_DEFAULT_H
#include <stddef.h>
#include <stdint.h>
typedef uint32_t nvm3_ObjectKey_t;
typedef struct { int dummy; } nvm3_Handle_t;
extern nvm3_Handle_t *nvm3_defaultHandle;
uint32_t nvm3_readPartialData(nvm3_Handle_t *h, nvm3_ObjectKey_t key, void *dst,
                              size_t off, size_t len);
uint32_t nvm3_getObjectInfo(nvm3_Handle_t *h, nvm3_ObjectKey_t key,
                            uint32_t *type, size_t *len);
#endif
