/*
 * SuperPSX – Memory Card Emulation
 *
 * Implements the PSX memory card SIO protocol per PSX-SPX documentation:
 *   https://psx-spx.consoledev.net/controllersandmemorycards/
 *
 * Protocol (Read as example):
 *   Byte 1:  TX=81h  RX=FF       Memory card access
 *   Byte 2:  TX=52h  RX=FLAG     Command (R/W/S), receive FLAG byte
 *   Byte 3:  TX=00h  RX=5Ah      Memory Card ID1
 *   Byte 4:  TX=00h  RX=5Dh      Memory Card ID2
 *   Byte 5:  TX=MSB  RX=00h      Address high byte
 *   Byte 6:  TX=LSB  RX=00h      Address low byte
 *   Byte 7:  TX=00h  RX=5Ch      ACK1
 *   Byte 8:  TX=00h  RX=5Dh      ACK2
 *   Byte 9:  TX=00h  RX=MSB      Confirmed address MSB
 *   Byte 10: TX=00h  RX=LSB      Confirmed address LSB
 *   Bytes 11-138: TX=00h RX=data 128 data bytes
 *   Byte 139: TX=00h RX=CHK      Checksum (MSB ^ LSB ^ all data bytes)
 *   Byte 140: TX=00h RX=47h      End byte (Good)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "memorycard.h"
#include "superpsx.h"
#include "config.h"

#define LOG_TAG "MCD"

/* ---- State machine phases ---- */
typedef enum {
    MCD_IDLE,       /* Waiting for 0x81 access byte */
    MCD_CMD,        /* Receive command (0x52/0x57/0x53), respond FLAG */
    MCD_ID1,        /* Respond 0x5A */
    MCD_ID2,        /* Respond 0x5D */
    /* Read/Write common */
    MCD_ADDR_MSB,   /* Receive address high byte */
    MCD_ADDR_LSB,   /* Receive address low byte */
    /* Read path */
    MCD_RD_ACK1,    /* Respond 0x5C */
    MCD_RD_ACK2,    /* Respond 0x5D */
    MCD_RD_ADDR_MSB,/* Respond confirmed address MSB */
    MCD_RD_ADDR_LSB,/* Respond confirmed address LSB */
    MCD_RD_DATA,    /* 128 data bytes */
    MCD_RD_CHK,     /* Respond checksum */
    MCD_RD_END,     /* Respond 0x47 (Good) */
    MCD_RD_COMPLETE,/* Post-read: absorb one extra byte before going idle.
                     * DuckStation sends ACK on ReadEnd (0x47), so the BIOS
                     * expects one more /IRQ7 after the last data byte.
                     * This state keeps IsIdle()==false for that ACK, then
                     * returns 0xFF + idle on the next (ignored) exchange. */
    /* Write path */
    MCD_WR_DATA,    /* 128 data bytes from host */
    MCD_WR_CHK,     /* Receive checksum from host */
    MCD_WR_ACK1,    /* Respond 0x5C */
    MCD_WR_ACK2,    /* Respond 0x5D */
    MCD_WR_END,     /* Respond 0x47/0x4E/0xFF */
    /* GetID path */
    MCD_ID_ACK1,    /* Respond 0x5C */
    MCD_ID_ACK2,    /* Respond 0x5D */
    MCD_ID_DATA,    /* 4 ID bytes: 04,00,00,80 */
} MCDPhase;

/* ---- Per-slot state ---- */
static struct {
    uint8_t data[MCD_SIZE];
    MCDPhase phase;
    uint8_t cmd;
    uint8_t flag;       /* FLAG byte (bit3=new_card, bit2=write_error) */
    uint16_t sector;    /* Current sector address */
    uint8_t checksum;   /* Running XOR checksum */
    int byte_count;     /* Byte counter within data phase */
    uint8_t wr_buf[MCD_SECTOR_SIZE]; /* Write staging buffer */
    int loaded;         /* 1 = card data loaded from file */
} cards[2];

/* ---- Format a blank memory card ---- */
static void mcd_format(int slot)
{
    memset(cards[slot].data, 0, MCD_SIZE);

    /* Block 0, Frame 0: Header */
    cards[slot].data[0] = 'M';
    cards[slot].data[1] = 'C';
    cards[slot].data[0x7F] = 0x0E; /* Checksum: 'M' ^ 'C' = 0x0E */

    /* Block 0, Frames 1-15: Directory entries */
    for (int i = 1; i <= 15; i++) {
        int off = i * MCD_SECTOR_SIZE;
        cards[slot].data[off] = 0xA0; /* Free, freshly formatted */
        /* Checksum for this frame: XOR of all 128 bytes */
        uint8_t xor = 0;
        for (int j = 0; j < MCD_SECTOR_SIZE; j++)
            xor ^= cards[slot].data[off + j];
        cards[slot].data[off + 0x7F] = xor;
    }

    /* Add a fake save in slot 1 (Directory Frame 1, Block 1) so BIOS shows something */
    {
        int off = 1 * MCD_SECTOR_SIZE; /* Frame 1 = first directory entry */
        cards[slot].data[off + 0] = 0x51; /* In use, first-or-only block */
        cards[slot].data[off + 1] = 0x00;
        cards[slot].data[off + 2] = 0x00;
        cards[slot].data[off + 3] = 0x00;
        /* Filesize: 1 block = 8192 bytes */
        cards[slot].data[off + 4] = 0x00;
        cards[slot].data[off + 5] = 0x20;
        cards[slot].data[off + 6] = 0x00;
        cards[slot].data[off + 7] = 0x00;
        /* Next block: 0xFFFF (last/only) */
        cards[slot].data[off + 8] = 0xFF;
        cards[slot].data[off + 9] = 0xFF;
        /* Filename: "BESCES-00000FAKE" */
        const char *fname = "BESCES-00000FAKE";
        for (int k = 0; fname[k]; k++)
            cards[slot].data[off + 0x0A + k] = (uint8_t)fname[k];
        /* Recompute checksum */
        uint8_t xor = 0;
        cards[slot].data[off + 0x7F] = 0;
        for (int j = 0; j < MCD_SECTOR_SIZE; j++)
            xor ^= cards[slot].data[off + j];
        cards[slot].data[off + 0x7F] = xor;

        /* Block 1, Frame 0 (sector 64): Title frame with icon */
        int boff = 64 * MCD_SECTOR_SIZE;
        /* SC magic + icon display flag (0x11 = 1 frame icon) */
        cards[slot].data[boff + 0] = 'S';
        cards[slot].data[boff + 1] = 'C';
        cards[slot].data[boff + 2] = 0x11; /* 1-frame icon */
        cards[slot].data[boff + 3] = 0x01; /* 1 block */
        /* Title: "SuperPSX Test" in Shift-JIS (ASCII subset) */
        const char *title = "SuperPSX Test";
        for (int k = 0; title[k]; k++)
            cards[slot].data[boff + 4 + k] = (uint8_t)title[k];
        /* Palette at 0x60 (16 entries x 2 bytes = 32 bytes) */
        /* Simple: color 0 = transparent (0x0000), color 1 = white (0x7FFF) */
        cards[slot].data[boff + 0x60] = 0x00;
        cards[slot].data[boff + 0x61] = 0x00;
        cards[slot].data[boff + 0x62] = 0xFF;
        cards[slot].data[boff + 0x63] = 0x7F;
        /* Icon data at 0x80 (16x16 pixels, 4bpp = 128 bytes) */
        /* Fill with color 1 (0x11 = two pixels of palette index 1) */
        memset(&cards[slot].data[boff + 0x80], 0x11, 128);
    }

    /* Block 0, Frames 16-35: Broken sector list (none) */
    for (int i = 16; i <= 35; i++) {
        int off = i * MCD_SECTOR_SIZE;
        cards[slot].data[off]     = 0xFF;
        cards[slot].data[off + 1] = 0xFF;
        cards[slot].data[off + 2] = 0xFF;
        cards[slot].data[off + 3] = 0xFF;
        /* Rest is 0x00 from memset */
        uint8_t xor = 0;
        for (int j = 0; j < MCD_SECTOR_SIZE; j++)
            xor ^= cards[slot].data[off + j];
        cards[slot].data[off + 0x7F] = xor;
    }

    /* Block 0, Frames 36-62: Unused (0xFF filled) */
    for (int i = 36; i <= 62; i++)
        memset(&cards[slot].data[i * MCD_SECTOR_SIZE], 0xFF, MCD_SECTOR_SIZE);

    /* Block 0, Frame 63: Write test frame (0xFF or 0x00, varies) */
    memset(&cards[slot].data[63 * MCD_SECTOR_SIZE], 0x00, MCD_SECTOR_SIZE);
}

/* ---- Save a single sector to file ---- */
static void mcd_save_sector(int slot, uint16_t sector)
{
    const char *path = (slot == 0) ? psx_config.mcd1_path : psx_config.mcd2_path;
    if (!path || !path[0]) return;

    FILE *f = fopen(path, "r+b");
    if (!f) {
        /* File doesn't exist, create it with full card data */
        f = fopen(path, "wb");
        if (!f) return;
        fwrite(cards[slot].data, 1, MCD_SIZE, f);
        fclose(f);
        return;
    }
    fseek(f, (long)sector * MCD_SECTOR_SIZE, SEEK_SET);
    fwrite(&cards[slot].data[sector * MCD_SECTOR_SIZE], 1, MCD_SECTOR_SIZE, f);
    fclose(f);
}

/* ---- Public: Initialize both memory card slots ---- */
void MCD_Init(void)
{
    for (int i = 0; i < 2; i++) {
        cards[i].phase = MCD_IDLE;
        cards[i].flag = 0x08; /* Bit3=1: directory not read yet */
        cards[i].loaded = 0;

        const char *path = (i == 0) ? psx_config.mcd1_path : psx_config.mcd2_path;
        if (!path || !path[0]) {
            DLOG("Slot %d: no path configured\n", i + 1);
            continue;
        }

        FILE *f = fopen(path, "rb");
        if (!f) {
            /* Create new formatted card */
            mcd_format(i);
            f = fopen(path, "wb");
            if (f) {
                fwrite(cards[i].data, 1, MCD_SIZE, f);
                fclose(f);
                printf("[MCD] Created new memory card %d: %s\n", i + 1, path);
            }
            cards[i].loaded = 1;
            continue;
        }

        size_t got = fread(cards[i].data, 1, MCD_SIZE, f);
        fclose(f);
        if (got == MCD_SIZE) {
            cards[i].loaded = 1;
            printf("[MCD] Loaded memory card %d: %s\n", i + 1, path);
        } else {
            printf("[MCD] WARNING: Short read for card %d (%zu bytes)\n", i + 1, got);
            if (got > 0)
                memset(cards[i].data + got, 0, MCD_SIZE - got);
            cards[i].loaded = 1;
        }
    }
}

/* ---- Public: Reset state machine when port is deselected ---- */
void MCD_Reset(int slot)
{
    if (slot >= 0 && slot < 2)
        cards[slot].phase = MCD_IDLE;
}

/* ---- Public: Check if card is idle (transfer aborted/completed) ---- */
int MCD_IsIdle(int slot)
{
    if (slot < 0 || slot >= 2) return 1;
    return cards[slot].phase == MCD_IDLE;
}

/* ---- Public: Check if card is loaded ---- */
int MCD_IsLoaded(int slot)
{
    if (slot < 0 || slot >= 2) return 0;
    return cards[slot].loaded;
}

/* ---- Public: Get card phase (debug) ---- */
int MCD_GetPhase(int slot)
{
    if (slot < 0 || slot >= 2) return -1;
    return (int)cards[slot].phase;
}

/* ---- Public: Check if card is in the data-transfer phase of a Read ----
 * Returns true when the memcard state machine is in a data-transfer phase
 * (MCD_RD_DATA, MCD_RD_CHK, MCD_RD_END, MCD_RD_COMPLETE).
 * Called BEFORE MCD_Tick to decide whether to defer the byte exchange. */
int MCD_InReadDataPhase(int slot)
{
    if (slot < 0 || slot >= 2) return 0;
    switch (cards[slot].phase) {
    case MCD_RD_DATA:
    case MCD_RD_CHK:
    case MCD_RD_END:
    case MCD_RD_COMPLETE:
        return 1;
    default:
        return 0;
    }
}

/* ---- Public: Process one byte exchange ----
 * tx = byte sent from PSX, returns byte received by PSX.
 * Returns 0xFF if no card present or in idle state. */
uint8_t MCD_Tick(int slot, uint8_t tx)
{
    if (slot < 0 || slot >= 2 || !cards[slot].loaded)
        return 0xFF;

    uint8_t rx = 0xFF;

    switch (cards[slot].phase) {

    /* ---- Byte 1: Access (0x81) ---- */
    case MCD_IDLE:
        if (tx == 0x81) {
            cards[slot].phase = MCD_CMD;
            rx = 0xFF; /* Dummy response */
        }
        break;

    /* ---- Byte 2: Command + FLAG response ---- */
    case MCD_CMD:
        rx = cards[slot].flag; /* FLAG byte — always returned per PSX-SPX */
        cards[slot].cmd = tx;
        if (tx == 0x52 || tx == 0x57) {
            /* Read or Write — next: ID1 */
            cards[slot].phase = MCD_ID1;
        } else if (tx == 0x53) {
            /* GetID — next: ID1 */
            cards[slot].phase = MCD_ID1;
        } else {
            /* Invalid command — return FLAG then abort.
             * PSX-SPX: "Transfer aborts immediately after the faulty command
             * byte, or, occasionally after one more byte (response FFh)." */
            cards[slot].phase = MCD_IDLE;
        }
        break;

    /* ---- Byte 3: ID1 (0x5A) ---- */
    case MCD_ID1:
        rx = 0x5A;
        cards[slot].phase = MCD_ID2;
        break;

    /* ---- Byte 4: ID2 (0x5D) ---- */
    case MCD_ID2:
        rx = 0x5D;
        if (cards[slot].cmd == 0x53) {
            /* GetID: skip address, go to ID ACK */
            cards[slot].phase = MCD_ID_ACK1;
        } else {
            cards[slot].phase = MCD_ADDR_MSB;
        }
        break;

    /* ---- Byte 5: Address MSB ---- */
    case MCD_ADDR_MSB:
        cards[slot].sector = (uint16_t)(tx << 8);
        rx = 0x00;
        cards[slot].phase = MCD_ADDR_LSB;
        break;

    /* ---- Byte 6: Address LSB ---- */
    case MCD_ADDR_LSB:
        cards[slot].sector |= tx;
        rx = 0x00; /* PSX-SPX: "pre" (previous byte echo), commonly 0 */
        cards[slot].checksum = (uint8_t)(cards[slot].sector >> 8) ^
                               (uint8_t)(cards[slot].sector & 0xFF);
        cards[slot].byte_count = 0;
        if (cards[slot].cmd == 0x52) {
            cards[slot].phase = MCD_RD_ACK1;
        } else {
            /* Write: data comes next */
            cards[slot].phase = MCD_WR_DATA;
        }
        break;

    /* ================================================ */
    /* ---- READ PATH ---- */
    /* ================================================ */

    /* ---- Byte 7: ACK1 (0x5C) ---- */
    case MCD_RD_ACK1:
        rx = 0x5C;
        cards[slot].phase = MCD_RD_ACK2;
        break;

    /* ---- Byte 8: ACK2 (0x5D) ---- */
    case MCD_RD_ACK2:
        rx = 0x5D;
        /* Check for invalid sector */
        if (cards[slot].sector >= MCD_NUM_SECTORS) {
            /* Sony cards respond 0xFFFF for confirmed addr, then abort */
            cards[slot].phase = MCD_RD_ADDR_MSB;
            cards[slot].sector = 0xFFFF; /* Will abort after addr echo */
        } else {
            cards[slot].phase = MCD_RD_ADDR_MSB;
        }
        break;

    /* ---- Byte 9: Confirmed address MSB ---- */
    case MCD_RD_ADDR_MSB:
        rx = (uint8_t)(cards[slot].sector >> 8);
        cards[slot].phase = MCD_RD_ADDR_LSB;
        break;

    /* ---- Byte 10: Confirmed address LSB ---- */
    case MCD_RD_ADDR_LSB:
        rx = (uint8_t)(cards[slot].sector & 0xFF);
        if (cards[slot].sector >= MCD_NUM_SECTORS) {
            /* Invalid sector: abort transfer */
            cards[slot].phase = MCD_IDLE;
        } else {
            cards[slot].phase = MCD_RD_DATA;
        }
        break;

    /* ---- Bytes 11-138: Data (128 bytes) ---- */
    case MCD_RD_DATA: {
        uint32_t off = (uint32_t)cards[slot].sector * MCD_SECTOR_SIZE +
                       cards[slot].byte_count;
        rx = cards[slot].data[off];
        cards[slot].checksum ^= rx;
        cards[slot].byte_count++;
        if (cards[slot].byte_count >= MCD_SECTOR_SIZE)
            cards[slot].phase = MCD_RD_CHK;
        break;
    }

    /* ---- Byte 139: Checksum ---- */
    case MCD_RD_CHK:
        rx = cards[slot].checksum;
        cards[slot].phase = MCD_RD_END;
        break;

    /* ---- Byte 140: End byte (0x47 = Good) ---- */
    case MCD_RD_END:
        rx = 0x47; /* 'G' = Good */
        /* DuckStation: ReadEnd sends ACK (ack=true) then goes Idle.
         * We go to MCD_RD_COMPLETE so IsIdle()==false → ACK is sent.
         * On the next byte (which the BIOS sends in response to ACK),
         * MCD_RD_COMPLETE returns 0xFF and transitions to IDLE (no ACK). */
        cards[slot].phase = MCD_RD_COMPLETE;
        break;

    /* ---- Post-read absorb byte (DuckStation ReadEnd ACK behavior) ---- */
    case MCD_RD_COMPLETE:
        rx = 0xFF;
        cards[slot].phase = MCD_IDLE;
        break;

    /* ================================================ */
    /* ---- WRITE PATH ---- */
    /* ================================================ */

    /* ---- Bytes 7-134: Data from host (128 bytes) ---- */
    case MCD_WR_DATA:
        cards[slot].wr_buf[cards[slot].byte_count] = tx;
        cards[slot].checksum ^= tx;
        rx = 0x00; /* PSX-SPX: "pre" */
        cards[slot].byte_count++;
        if (cards[slot].byte_count >= MCD_SECTOR_SIZE)
            cards[slot].phase = MCD_WR_CHK;
        break;

    /* ---- Byte 135: Checksum from host ---- */
    case MCD_WR_CHK: {
        uint8_t expected = cards[slot].checksum;
        if (tx == expected) {
            /* Checksum OK: commit write */
            if (cards[slot].sector < MCD_NUM_SECTORS) {
                memcpy(&cards[slot].data[cards[slot].sector * MCD_SECTOR_SIZE],
                       cards[slot].wr_buf, MCD_SECTOR_SIZE);
                mcd_save_sector(slot, cards[slot].sector);
                /* Clear FLAG bit3 (new_card) on successful write */
                cards[slot].flag &= ~0x08;
            }
        } else {
            /* Checksum mismatch: set FLAG bit2 (write error) */
            cards[slot].flag |= 0x04;
        }
        rx = 0x00;
        cards[slot].phase = MCD_WR_ACK1;
        break;
    }

    /* ---- Byte 136: ACK1 (0x5C) ---- */
    case MCD_WR_ACK1:
        rx = 0x5C;
        cards[slot].phase = MCD_WR_ACK2;
        break;

    /* ---- Byte 137: ACK2 (0x5D) ---- */
    case MCD_WR_ACK2:
        rx = 0x5D;
        cards[slot].phase = MCD_WR_END;
        break;

    /* ---- Byte 138: End byte ---- */
    case MCD_WR_END: {
        /* Determine result based on previous checksum comparison */
        if (cards[slot].sector >= MCD_NUM_SECTORS)
            rx = 0xFF; /* Bad sector */
        else if (cards[slot].flag & 0x04)
            rx = 0x4E; /* Bad checksum */
        else
            rx = 0x47; /* Good */
        cards[slot].phase = MCD_IDLE;
        break;
    }

    /* ================================================ */
    /* ---- GETID PATH ---- */
    /* ================================================ */

    case MCD_ID_ACK1:
        rx = 0x5C;
        cards[slot].phase = MCD_ID_ACK2;
        break;

    case MCD_ID_ACK2:
        rx = 0x5D;
        cards[slot].byte_count = 0;
        cards[slot].phase = MCD_ID_DATA;
        break;

    case MCD_ID_DATA: {
        /* 4 bytes: 04h, 00h, 00h, 80h */
        static const uint8_t id_data[4] = {0x04, 0x00, 0x00, 0x80};
        rx = id_data[cards[slot].byte_count];
        cards[slot].byte_count++;
        if (cards[slot].byte_count >= 4)
            cards[slot].phase = MCD_IDLE;
        break;
    }

    } /* switch */

    return rx;
}
