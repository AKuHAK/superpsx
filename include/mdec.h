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

#ifdef ENABLE_MDEC_IPU
/* IPU hardware-accelerated decode (Phase 2) */
void MDEC_IPU_Init(void);
void MDEC_IPU_LoadQuantTable(const uint8_t *qt_y, const uint8_t *qt_uv);
int  MDEC_IPU_DecodeDMA1(const uint16_t **rl, const uint16_t *rl_end,
                          uint8_t *image, uint32_t size,
                          int depth, int signed_out, int stp_bit);
#endif

#endif
