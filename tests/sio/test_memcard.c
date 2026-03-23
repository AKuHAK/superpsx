/*
 * test_memcard.c — Host-side TDD tests for Memory Card protocol
 *
 * Verifies the MCD_Tick() state machine byte-by-byte against PSX-SPX spec.
 * Compile and run on host (not PS2):
 *   cc -I../../include -DTEST_MEMCARD -o test_memcard test_memcard.c && ./test_memcard
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

/* ---- Mock dependencies ---- */
#define LOG_TAG "MCD"
/* Suppress DLOG before superpsx.h defines it */
#define VERBOSE 0
#include "config.h"
#include "superpsx.h"
PSXConfig psx_config;
void SignalInterrupt(uint32_t irq) { (void)irq; }

/* Include the actual implementation */
#include "../../src/memorycard.c"

/* ---- Test infrastructure ---- */
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(a, b, msg) do {                                      \
    uint8_t _a = (a), _b = (b);                                        \
    if (_a != _b) {                                                     \
        printf("  FAIL: %s: got 0x%02X, expected 0x%02X\n", msg, _a, _b); \
        tests_failed++;                                                 \
        return;                                                         \
    }                                                                   \
} while (0)

#define PASS(name) do { tests_passed++; printf("  PASS: %s\n", name); } while(0)

/* ---- Helper: simulate one SIO byte exchange ---- */
static uint8_t tick(int slot, uint8_t tx)
{
    return MCD_Tick(slot, tx);
}

/* ---- Helper: init cards with no file I/O (in-memory only) ---- */
static void init_cards_memory_only(void)
{
    /* Clear config paths so no file I/O happens */
    memset(psx_config.mcd1_path, 0, sizeof(psx_config.mcd1_path));
    memset(psx_config.mcd2_path, 0, sizeof(psx_config.mcd2_path));

    /* Manually init card 0 in memory */
    MCD_Init(); /* Will skip file I/O due to empty paths */

    /* Force card 0 as loaded with formatted data */
    cards[0].loaded = 1;
    cards[0].phase = MCD_IDLE;
    cards[0].flag = 0x08;
    mcd_format(0);

    /* Force card 1 as loaded */
    cards[1].loaded = 1;
    cards[1].phase = MCD_IDLE;
    cards[1].flag = 0x08;
    mcd_format(1);
}

/* ============================================================ */
/* TEST: Read command — full 140-byte protocol */
/* ============================================================ */
static void test_read_protocol(void)
{
    init_cards_memory_only();

    /* Write known data to sector 0 for verification */
    for (int i = 0; i < 128; i++)
        cards[0].data[i] = (uint8_t)i;

    uint8_t rx;

    /* Byte 1: Access */
    rx = tick(0, 0x81);
    ASSERT_EQ(rx, 0xFF, "read: byte 1 (access)");

    /* Byte 2: Command 0x52 (Read), expect FLAG */
    rx = tick(0, 0x52);
    ASSERT_EQ(rx, 0x08, "read: byte 2 (FLAG)");

    /* Byte 3: ID1 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5A, "read: byte 3 (ID1)");

    /* Byte 4: ID2 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5D, "read: byte 4 (ID2)");

    /* Byte 5: Address MSB (sector 0 = 0x0000) */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x00, "read: byte 5 (addr MSB)");

    /* Byte 6: Address LSB */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x00, "read: byte 6 (addr LSB)");

    /* Byte 7: ACK1 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5C, "read: byte 7 (ACK1)");

    /* Byte 8: ACK2 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5D, "read: byte 8 (ACK2)");

    /* Byte 9: Confirmed address MSB */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x00, "read: byte 9 (conf MSB)");

    /* Byte 10: Confirmed address LSB */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x00, "read: byte 10 (conf LSB)");

    /* Bytes 11-138: Data (128 bytes) */
    uint8_t checksum = 0x00 ^ 0x00; /* MSB ^ LSB */
    for (int i = 0; i < 128; i++) {
        rx = tick(0, 0x00);
        ASSERT_EQ(rx, (uint8_t)i, "read: data byte");
        checksum ^= rx;
    }

    /* Byte 139: Checksum */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, checksum, "read: checksum");

    /* Byte 140: End byte (0x47 = Good) */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x47, "read: end byte");

    PASS("test_read_protocol");
}

/* ============================================================ */
/* TEST: Read sector 0x0123 — verify address echo */
/* ============================================================ */
static void test_read_sector_addr_echo(void)
{
    init_cards_memory_only();

    tick(0, 0x81); /* Access */
    tick(0, 0x52); /* Read */
    tick(0, 0x00); /* ID1 */
    tick(0, 0x00); /* ID2 */
    tick(0, 0x01); /* Addr MSB = 0x01 */
    tick(0, 0x23); /* Addr LSB = 0x23 */
    tick(0, 0x00); /* ACK1 */
    tick(0, 0x00); /* ACK2 */

    uint8_t msb = tick(0, 0x00); /* Conf MSB */
    ASSERT_EQ(msb, 0x01, "addr echo: MSB");

    uint8_t lsb = tick(0, 0x00); /* Conf LSB */
    ASSERT_EQ(lsb, 0x23, "addr echo: LSB");

    /* Skip rest of data + chk + end */
    for (int i = 0; i < 128 + 2; i++)
        tick(0, 0x00);

    PASS("test_read_sector_addr_echo");
}

/* ============================================================ */
/* TEST: Write command — full protocol with checksum ok */
/* ============================================================ */
static void test_write_protocol(void)
{
    init_cards_memory_only();

    /* We'll write to sector 5 */
    uint8_t rx;

    /* Byte 1: Access */
    rx = tick(0, 0x81);
    ASSERT_EQ(rx, 0xFF, "write: byte 1");

    /* Byte 2: Command 0x57 (Write) */
    rx = tick(0, 0x57);
    ASSERT_EQ(rx, 0x08, "write: byte 2 (FLAG)");

    /* Byte 3: ID1 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5A, "write: byte 3 (ID1)");

    /* Byte 4: ID2 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5D, "write: byte 4 (ID2)");

    /* Byte 5: Addr MSB (sector 5 = 0x0005) */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x00, "write: byte 5 (addr MSB)");

    /* Byte 6: Addr LSB */
    rx = tick(0, 0x05);
    ASSERT_EQ(rx, 0x00, "write: byte 6 (addr LSB)");

    /* Bytes 7-134: Send 128 data bytes */
    uint8_t checksum = 0x00 ^ 0x05; /* MSB ^ LSB */
    for (int i = 0; i < 128; i++) {
        uint8_t b = (uint8_t)(0xA0 + i);
        rx = tick(0, b);
        /* rx is "pre" (0x00 typically) */
        checksum ^= b;
    }

    /* Byte 135: Send checksum */
    rx = tick(0, checksum);
    ASSERT_EQ(rx, 0x00, "write: chk response");

    /* Byte 136: ACK1 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5C, "write: ACK1");

    /* Byte 137: ACK2 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5D, "write: ACK2");

    /* Byte 138: End byte (0x47 = Good) */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x47, "write: end byte");

    /* Verify data was actually written to card */
    for (int i = 0; i < 128; i++) {
        uint8_t expected = (uint8_t)(0xA0 + i);
        uint8_t actual = cards[0].data[5 * 128 + i];
        if (actual != expected) {
            printf("  FAIL: write verify: byte %d got 0x%02X expected 0x%02X\n",
                   i, actual, expected);
            tests_failed++;
            return;
        }
    }

    PASS("test_write_protocol");
}

/* ============================================================ */
/* TEST: Write with bad checksum → 0x4E end byte */
/* ============================================================ */
static void test_write_bad_checksum(void)
{
    init_cards_memory_only();

    tick(0, 0x81); /* Access */
    tick(0, 0x57); /* Write */
    tick(0, 0x00); /* ID1 */
    tick(0, 0x00); /* ID2 */
    tick(0, 0x00); /* Addr MSB (sector 0) */
    tick(0, 0x02); /* Addr LSB (sector 2) */

    /* Send 128 bytes of 0x00 */
    for (int i = 0; i < 128; i++)
        tick(0, 0x00);

    /* Send WRONG checksum (correct would be 0x00^0x02 = 0x02) */
    tick(0, 0xFF);

    tick(0, 0x00); /* ACK1 = 0x5C */
    tick(0, 0x00); /* ACK2 = 0x5D */

    uint8_t rx = tick(0, 0x00); /* End byte */
    ASSERT_EQ(rx, 0x4E, "bad checksum: end byte 0x4E");

    PASS("test_write_bad_checksum");
}

/* ============================================================ */
/* TEST: FLAG cleared after successful write */
/* ============================================================ */
static void test_flag_cleared_on_write(void)
{
    init_cards_memory_only();
    /* Initial FLAG should be 0x08 (new card) */

    /* Do a write to sector 1 */
    tick(0, 0x81);
    uint8_t flag_before = tick(0, 0x57); /* FLAG = 0x08 */
    ASSERT_EQ(flag_before, 0x08, "flag: initial 0x08");

    tick(0, 0x00); /* ID1 */
    tick(0, 0x00); /* ID2 */
    tick(0, 0x00); /* Addr MSB */
    tick(0, 0x01); /* Addr LSB = 1 */

    /* Send 128 bytes of 0x55 */
    uint8_t checksum = 0x00 ^ 0x01;
    for (int i = 0; i < 128; i++) {
        tick(0, 0x55);
        checksum ^= 0x55;
    }
    tick(0, checksum); /* Correct checksum */
    tick(0, 0x00); /* ACK1 */
    tick(0, 0x00); /* ACK2 */
    tick(0, 0x00); /* End = 0x47 */

    /* Now do a read — FLAG should be 0x00 (bit3 cleared by write) */
    tick(0, 0x81);
    uint8_t flag_after = tick(0, 0x52);
    ASSERT_EQ(flag_after, 0x00, "flag: cleared after write");

    /* Clean up: skip rest of read */
    MCD_Reset(0);

    PASS("test_flag_cleared_on_write");
}

/* ============================================================ */
/* TEST: GetID command (0x53) */
/* ============================================================ */
static void test_getid_protocol(void)
{
    init_cards_memory_only();
    uint8_t rx;

    /* Byte 1: Access */
    rx = tick(0, 0x81);
    ASSERT_EQ(rx, 0xFF, "getid: byte 1");

    /* Byte 2: Command 0x53, expect FLAG */
    rx = tick(0, 0x53);
    ASSERT_EQ(rx, 0x08, "getid: byte 2 (FLAG)");

    /* Byte 3: ID1 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5A, "getid: byte 3 (ID1)");

    /* Byte 4: ID2 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5D, "getid: byte 4 (ID2)");

    /* Byte 5: ACK1 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5C, "getid: byte 5 (ACK1)");

    /* Byte 6: ACK2 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x5D, "getid: byte 6 (ACK2)");

    /* Byte 7: 0x04 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x04, "getid: byte 7");

    /* Byte 8: 0x00 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x00, "getid: byte 8");

    /* Byte 9: 0x00 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x00, "getid: byte 9");

    /* Byte 10: 0x80 */
    rx = tick(0, 0x00);
    ASSERT_EQ(rx, 0x80, "getid: byte 10");

    PASS("test_getid_protocol");
}

/* ============================================================ */
/* TEST: MCD_Reset() returns to idle */
/* ============================================================ */
static void test_reset(void)
{
    init_cards_memory_only();

    /* Start a read, then reset mid-way */
    tick(0, 0x81);
    tick(0, 0x52);
    tick(0, 0x00); /* ID1 */

    /* Reset should return to idle */
    MCD_Reset(0);

    /* Now a new access should work from scratch */
    uint8_t rx = tick(0, 0x81);
    ASSERT_EQ(rx, 0xFF, "reset: new access after reset");

    rx = tick(0, 0x52);
    ASSERT_EQ(rx, 0x08, "reset: new read FLAG");

    MCD_Reset(0);
    PASS("test_reset");
}

/* ============================================================ */
/* TEST: No card present (not loaded) returns 0xFF */
/* ============================================================ */
static void test_no_card(void)
{
    init_cards_memory_only();
    cards[0].loaded = 0; /* Simulate no card */

    uint8_t rx = tick(0, 0x81);
    ASSERT_EQ(rx, 0xFF, "no card: returns 0xFF");

    rx = tick(0, 0x52);
    ASSERT_EQ(rx, 0xFF, "no card: still 0xFF");

    /* Restore for other tests */
    cards[0].loaded = 1;
    PASS("test_no_card");
}

/* ============================================================ */
/* TEST: Write then read back — round-trip */
/* ============================================================ */
static void test_write_read_roundtrip(void)
{
    init_cards_memory_only();

    uint8_t write_data[128];
    for (int i = 0; i < 128; i++)
        write_data[i] = (uint8_t)(0x30 + (i % 10)); /* "0123456789..." */

    /* ---- WRITE to sector 10 ---- */
    tick(0, 0x81);
    tick(0, 0x57); /* Write */
    tick(0, 0x00); /* ID1 */
    tick(0, 0x00); /* ID2 */
    tick(0, 0x00); /* Addr MSB */
    tick(0, 0x0A); /* Addr LSB = 10 */

    uint8_t checksum = 0x00 ^ 0x0A;
    for (int i = 0; i < 128; i++) {
        tick(0, write_data[i]);
        checksum ^= write_data[i];
    }
    tick(0, checksum);
    tick(0, 0x00); /* ACK1 */
    tick(0, 0x00); /* ACK2 */
    uint8_t end = tick(0, 0x00);
    ASSERT_EQ(end, 0x47, "roundtrip: write end");

    /* ---- READ back sector 10 ---- */
    tick(0, 0x81);
    tick(0, 0x52); /* Read */
    tick(0, 0x00); /* ID1 */
    tick(0, 0x00); /* ID2 */
    tick(0, 0x00); /* Addr MSB */
    tick(0, 0x0A); /* Addr LSB = 10 */
    tick(0, 0x00); /* ACK1 */
    tick(0, 0x00); /* ACK2 */
    tick(0, 0x00); /* Conf MSB */
    tick(0, 0x00); /* Conf LSB */

    for (int i = 0; i < 128; i++) {
        uint8_t rx = tick(0, 0x00);
        if (rx != write_data[i]) {
            printf("  FAIL: roundtrip: byte %d got 0x%02X expected 0x%02X\n",
                   i, rx, write_data[i]);
            tests_failed++;
            return;
        }
    }

    tick(0, 0x00); /* Checksum */
    end = tick(0, 0x00);
    ASSERT_EQ(end, 0x47, "roundtrip: read end");

    PASS("test_write_read_roundtrip");
}

/* ============================================================ */
/* TEST: Invalid command returns 0xFF and goes idle */
/* ============================================================ */
static void test_invalid_command(void)
{
    init_cards_memory_only();

    tick(0, 0x81); /* Access */
    uint8_t rx = tick(0, 0xAA); /* Invalid command */
    ASSERT_EQ(rx, 0x08, "invalid cmd: returns FLAG byte");

    /* Should be back in idle — new access should work */
    rx = tick(0, 0x81);
    ASSERT_EQ(rx, 0xFF, "invalid cmd: re-access works");

    rx = tick(0, 0x52);
    ASSERT_EQ(rx, 0x08, "invalid cmd: read after invalid");

    MCD_Reset(0);
    PASS("test_invalid_command");
}

/* ============================================================ */
/* TEST: Slot 1 (second card) works independently */
/* ============================================================ */
static void test_slot1_independent(void)
{
    init_cards_memory_only();

    /* Write different data to slot 0 sector 0 and slot 1 sector 0 */
    memset(cards[0].data, 0xAA, 128);
    memset(cards[1].data, 0xBB, 128);

    /* Read from slot 0 */
    tick(0, 0x81);
    tick(0, 0x52);
    tick(0, 0x00);
    tick(0, 0x00);
    tick(0, 0x00);
    tick(0, 0x00);
    tick(0, 0x00); /* ACK1 */
    tick(0, 0x00); /* ACK2 */
    tick(0, 0x00); /* Conf MSB */
    tick(0, 0x00); /* Conf LSB */
    uint8_t slot0_byte = tick(0, 0x00);
    ASSERT_EQ(slot0_byte, 0xAA, "slot independence: slot 0");
    MCD_Reset(0);

    /* Read from slot 1 */
    tick(1, 0x81);
    tick(1, 0x52);
    tick(1, 0x00);
    tick(1, 0x00);
    tick(1, 0x00);
    tick(1, 0x00);
    tick(1, 0x00); /* ACK1 */
    tick(1, 0x00); /* ACK2 */
    tick(1, 0x00); /* Conf MSB */
    tick(1, 0x00); /* Conf LSB */
    uint8_t slot1_byte = tick(1, 0x00);
    ASSERT_EQ(slot1_byte, 0xBB, "slot independence: slot 1");
    MCD_Reset(1);

    PASS("test_slot1_independent");
}

/* ============================================================ */
/* TEST: Read out-of-range sector (>= 1024) */
/* ============================================================ */
static void test_read_out_of_range(void)
{
    init_cards_memory_only();

    tick(0, 0x81);
    tick(0, 0x52); /* Read */
    tick(0, 0x00); /* ID1 */
    tick(0, 0x00); /* ID2 */
    tick(0, 0xFF); /* Addr MSB (sector 0xFF00 > 1024) */
    tick(0, 0x00); /* Addr LSB */
    tick(0, 0x00); /* ACK1 */
    tick(0, 0x00); /* ACK2 */

    /* Confirmed address should echo back 0xFFFF (invalid) */
    uint8_t msb = tick(0, 0x00);
    ASSERT_EQ(msb, 0xFF, "out-of-range: conf MSB=0xFF");
    uint8_t lsb = tick(0, 0x00);
    ASSERT_EQ(lsb, 0xFF, "out-of-range: conf LSB=0xFF");

    /* After invalid sector, should be back to idle */
    uint8_t rx = tick(0, 0x81);
    ASSERT_EQ(rx, 0xFF, "out-of-range: back to idle");

    MCD_Reset(0);
    PASS("test_read_out_of_range");
}

/* ============================================================ */
/* TEST: Formatted card header is valid */
/* ============================================================ */
static void test_formatted_header(void)
{
    init_cards_memory_only();

    /* Read sector 0 — should have MC header */
    tick(0, 0x81);
    tick(0, 0x52);
    tick(0, 0x00);
    tick(0, 0x00);
    tick(0, 0x00); /* Sector 0 MSB */
    tick(0, 0x00); /* Sector 0 LSB */
    tick(0, 0x00); /* ACK1 */
    tick(0, 0x00); /* ACK2 */
    tick(0, 0x00); /* Conf MSB */
    tick(0, 0x00); /* Conf LSB */

    uint8_t b0 = tick(0, 0x00); /* data[0] = 'M' */
    ASSERT_EQ(b0, 'M', "format: header byte 0 = 'M'");
    uint8_t b1 = tick(0, 0x00); /* data[1] = 'C' */
    ASSERT_EQ(b1, 'C', "format: header byte 1 = 'C'");

    MCD_Reset(0);
    PASS("test_formatted_header");
}

/* ============================================================ */
/* MAIN */
/* ============================================================ */
int main(void)
{
    printf("=== Memory Card Protocol Tests ===\n\n");

    test_read_protocol();
    test_read_sector_addr_echo();
    test_write_protocol();
    test_write_bad_checksum();
    test_flag_cleared_on_write();
    test_getid_protocol();
    test_reset();
    test_no_card();
    test_write_read_roundtrip();
    test_invalid_command();
    test_slot1_independent();
    test_read_out_of_range();
    test_formatted_header();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    return tests_failed ? 1 : 0;
}
