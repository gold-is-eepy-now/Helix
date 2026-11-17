/* aml-unit.c — host tests for the AML interpreter. The bytecode below is hand-assembled from
 * the ACPI 6.x encodings (the comment on each block shows the ASL it stands for), so the test
 * does not depend on any firmware. Region reads and writes go to a fake memory array.
 * Build and run: see the aml-unit line in the Makefile. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "aml.h"
#include "acpi.h"
#include "kstring.h"

/* ---- stubs for what the kernel provides ---- */

void *kmalloc(size_t n) { return calloc(1, n ? n : 1); }
void kfree(void *p) { free(p); }

uint64_t time_tsc_per_ms(void) { return 0; }   /* no calibration: busy waits return at once */
uint64_t time_tsc(void) { return 0; }

/* No firmware tables in the host test: the blocks are loaded with aml_load_block. */
const void *acpi_find(const char sig[4]) { (void)sig; return NULL; }
unsigned acpi_table_count(void) { return 0; }
bool acpi_table_at(unsigned i, acpi_table_t *out) { (void)i; (void)out; return false; }
const void *acpi_table_data(unsigned i) { (void)i; return NULL; }
uint64_t acpi_hhdm(void) { return 0; }

/* Fake physical memory for the SystemMemory region tests. */
static uint8_t g_mem[0x1000];
aml_status_t aml_region_read8(uint8_t space, uint64_t addr, uint8_t *out) {
    if (space != 0 || addr >= sizeof(g_mem)) return AML_ERR_HARDWARE;
    *out = g_mem[addr];
    return AML_OK;
}
aml_status_t aml_region_write8(uint8_t space, uint64_t addr, uint8_t value) {
    if (space != 0 || addr >= sizeof(g_mem)) return AML_ERR_HARDWARE;
    g_mem[addr] = value;
    return AML_OK;
}

/* ---- the test table ---- */

static int g_fail;
static void check(int ok, const char *what) {
    printf("aml-unit: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

#define BODY_MAX 1024
static uint8_t g_table[BODY_MAX];
static uint32_t g_table_len;

static void emit(const void *bytes, uint32_t n) {
    memcpy(g_table + g_table_len, bytes, n);
    g_table_len += n;
}
#define B(...) emit((const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }))
#define N(s) emit(s, 4)

static void build_table(void) {
    g_table_len = 36;
    memset(g_table, 0, sizeof(g_table));

    /* Name(VAL_, 0x2A) */
    B(0x08); N("VAL_"); B(0x0A, 0x2A);

    /* Device(DEV0) { Name(_HID, EisaId("PNP0A08")) }: the EISA id is the integer 0x080AD041. */
    B(0x5B, 0x82, 0x0F); N("DEV0");
    B(0x08); N("_HID"); B(0x0C, 0x41, 0xD0, 0x0A, 0x08);

    /* Method(ADD2, 2) { Return(Add(Arg0, Arg1, Local0)) } */
    B(0x14, 0x0B); N("ADD2"); B(0x02, 0xA4, 0x72, 0x68, 0x69, 0x60);

    /* Method(LOOP) { Local0 = 0; Local1 = 0; While (Local1 < 5) { Increment(Local1); Local0 += Local1 }; Return(Local0) } */
    B(0x14, 0x1A); N("LOOP"); B(0x00,
        0x70, 0x00, 0x60,
        0x70, 0x00, 0x61,
        0xA2, 0x0B, 0x95, 0x61, 0x0A, 0x05, 0x75, 0x61, 0x72, 0x60, 0x61, 0x60,
        0xA4, 0x60);

    /* OperationRegion(MEMR, SystemMemory, 0x100, 0x10); Field(MEMR, ByteAcc) { BYT0, 8, BYT1, 8 } */
    B(0x5B, 0x80); N("MEMR"); B(0x00, 0x0B, 0x00, 0x01, 0x0A, 0x10);
    B(0x5B, 0x81, 0x10); N("MEMR"); B(0x01); N("BYT0"); B(0x08); N("BYT1"); B(0x08);

    /* Method(READ0) { Return(BYT0) } */
    B(0x14, 0x0B); N("READ"); B(0x00, 0xA4); N("BYT0");

    /* Method(WRIT) { BYT1 = 0x77; Return(BYT1) } */
    B(0x14, 0x12); N("WRIT"); B(0x00, 0x70, 0x0A, 0x77); N("BYT1"); B(0xA4); N("BYT1");

    /* Method(IFT_, 1) { If (Arg0 == 3) { Return(1) } Else { Return(2) } } */
    B(0x14, 0x13); N("IFT_"); B(0x01,
        0xA0, 0x07, 0x93, 0x68, 0x0A, 0x03, 0xA4, 0x01,
        0xA1, 0x04, 0xA4, 0x0A, 0x02);

    /* Name(PKG_, Package(2) { 1, 2 }) and Method(PICK) { Return(Index(PKG_, 1, NullName)) } */
    B(0x08); N("PKG_"); B(0x12, 0x05, 0x02, 0x01, 0x0A, 0x02);
    B(0x14, 0x0F); N("PICK"); B(0x00, 0xA4, 0x88); N("PKG_"); B(0x0A, 0x01, 0x00);

    /* Method(INF_) { While (One) { } } : runs forever, so the budget must stop it. */
    B(0x14, 0x09); N("INF_"); B(0x00, 0xA2, 0x02, 0x01);

    /* Method(MODT) { Return(Mod(10, 3, Local0)) }: 10 mod 3 = 1. */
    B(0x14, 0x0D); N("MODT"); B(0x00, 0xA4, 0x85, 0x0A, 0x0A, 0x0A, 0x03, 0x60);

    /* Fill in the header: signature, length, revision 2. */
    memcpy(g_table, "DSDT", 4);
    g_table[4] = (uint8_t)g_table_len;
    g_table[5] = (uint8_t)(g_table_len >> 8);
    g_table[6] = (uint8_t)(g_table_len >> 16);
    g_table[7] = (uint8_t)(g_table_len >> 24);
    g_table[8] = 2;
}

static aml_status_t call_int(const char *path, const aml_value_t *args, unsigned nargs, uint64_t *out) {
    aml_value_t v;
    aml_status_t st = aml_eval(path, args, nargs, &v);
    if (st == AML_OK) {
        if (v.type != AML_T_INT) st = AML_ERR_TYPE;
        else *out = v.integer;
        aml_value_free(&v);
    }
    return st;
}

int main(void) {
    build_table();
    /* A table whose header says it is longer than the bytes given must be refused. */
    check(aml_load_block(g_table, 20) == AML_ERR_BAD_CODE, "a short table is refused");

    aml_status_t st = aml_load_block(g_table, g_table_len);
    check(st == AML_OK, "the test definition block loads");
    if (st != AML_OK) {
        printf("aml-unit: load said %s\n", aml_status_name(st));
        return 1;
    }

    uint64_t v = 0;
    check(call_int("\\VAL_", NULL, 0, &v) == AML_OK && v == 0x2A, "Name(VAL_, 0x2A) reads back 0x2A");

    char hid[9] = "";
    check(aml_get_hid("\\DEV0", hid) && strcmp(hid, "PNP0A08") == 0, "Device(DEV0)._HID decodes to PNP0A08");

    aml_value_t args[2];
    args[0].type = AML_T_INT; args[0].integer = 40;
    args[1].type = AML_T_INT; args[1].integer = 2;
    check(call_int("\\ADD2", args, 2, &v) == AML_OK && v == 42, "ADD2(40, 2) returns 42");

    check(call_int("\\LOOP", NULL, 0, &v) == AML_OK && v == 15, "LOOP sums 1..5 with a While loop to 15");

    g_mem[0x100] = 0x5A;
    g_mem[0x101] = 0x33;
    check(call_int("\\READ", NULL, 0, &v) == AML_OK && v == 0x5A, "a field reads the region byte (0x5A)");

    check(call_int("\\WRIT", NULL, 0, &v) == AML_OK && v == 0x77, "WRIT stores 0x77 into a field and returns it");
    check(g_mem[0x101] == 0x77, "the store reached the region byte in fake memory");

    args[0].integer = 3;
    check(call_int("\\IFT_", args, 1, &v) == AML_OK && v == 1, "IFT_(3) takes the If branch and returns 1");
    args[0].integer = 4;
    check(call_int("\\IFT_", args, 1, &v) == AML_OK && v == 2, "IFT_(4) takes the Else branch and returns 2");

    check(call_int("\\PICK", NULL, 0, &v) == AML_OK && v == 2, "Index(PKG_, 1) returns the second element (2)");

    check(call_int("\\MODT", NULL, 0, &v) == AML_OK && v == 1, "Mod(10, 3) returns 1");

    check(call_int("\\INF_", NULL, 0, &v) == AML_ERR_BUDGET, "an endless While stops at the step budget");

    check(call_int("\\NOPE", NULL, 0, &v) == AML_ERR_NOT_FOUND, "an unknown path is not found");

    printf("aml-unit: %s (%d failure(s))\n", g_fail ? "FAILED" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
