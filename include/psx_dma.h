#ifndef DMA_H
#define DMA_H

#include <stdint.h>

uint32_t DMA_Read(uint32_t addr);
void DMA_Write(uint32_t addr, uint32_t data);

/* Returns non-zero when a deferred DMA is in progress (for idle-skip suppression). */
int DMA_IsPending(void);

#endif
