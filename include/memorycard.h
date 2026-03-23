#ifndef MEMORYCARD_H
#define MEMORYCARD_H

#include <stdint.h>

#define MCD_SECTOR_SIZE 128
#define MCD_NUM_SECTORS 1024
#define MCD_SIZE (MCD_NUM_SECTORS * MCD_SECTOR_SIZE) /* 128 KB */

void MCD_Init(void);
void MCD_Reset(int slot);
uint8_t MCD_Tick(int slot, uint8_t tx);
int MCD_IsIdle(int slot);
int MCD_IsLoaded(int slot);
int MCD_GetPhase(int slot);
int MCD_InReadDataPhase(int slot);

#endif /* MEMORYCARD_H */
