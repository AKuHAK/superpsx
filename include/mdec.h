#ifndef MDEC_H
#define MDEC_H

#include <stdint.h>

void MDEC_Init(void);
void MDEC_WriteCommand(uint32_t data);  /* 0x1F801820 write */
void MDEC_WriteControl(uint32_t data);  /* 0x1F801824 write */
uint32_t MDEC_ReadData(void);           /* 0x1F801820 read  */
uint32_t MDEC_ReadStatus(void);         /* 0x1F801824 read  */

/* DMA channel handlers */
void MDEC_DMA0(uint32_t madr, uint32_t bcr, uint32_t chcr); /* MDECin:  RAM→MDEC */
void MDEC_DMA1(uint32_t madr, uint32_t bcr, uint32_t chcr); /* MDECout: MDEC→RAM */

/* Scheduler callback for deferred DMA1 completion */
void MDEC_DMA1_Complete(void);

#endif
