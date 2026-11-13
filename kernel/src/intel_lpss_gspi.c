/* intel_lpss_gspi.c — polled, programmed-I/O driver for the Intel LPSS GSPI controller.
 *
 * Register sources (Linux kernel, GPL-2.0), fetched and read during this work:
 *   SSP registers and bits: include/linux/pxa2xx_ssp.h (SSCR0 0x00, SSCR1 0x04,
 *     SSSR 0x08, SSDR 0x10; SSCR0_SSE bit 7, SSCR0_SCR at bit 8, SSCR0_DSS mask 0xF,
 *     SSCR1_SPO bit 3, SSCR1_SPH bit 4, SSSR_TNF bit 2, SSSR_RNE bit 3, SSSR_BSY bit 4,
 *     SSSR_ROR bit 7).
 *   LPSS private block, 0x200 into BAR0: drivers/mfd/intel-lpss.c (LPSS_PRIV_OFFSET 0x200,
 *     LPSS_PRIV_RESETS 0x04, RESETS_FUNC 0x3, RESETS_IDMA BIT(2); reset sequence:
 *     write 0, then write FUNC|IDMA).
 *
 * NOT verified, so not used here: the clock divider's exact formula and input clock (the
 * private block's divider register is at 0x200 with its gate at bit 0 and update at bit
 * 31, from the same file, but the rate arithmetic is unconfirmed); the FIFO depth (we poll
 * TNF and RNE per byte instead of assuming a depth); the LPSS remap address register and
 * DMA-finish bit (their offsets are not in the sources we read); chip-select framing (the
 * controller's own SFRM behaviour is not verified); and the device's own protocol.
 *
 * The SSP rate is input / (2 * (SCR + 1)). The input clock is unverified, so the driver
 * programs the slowest SCR (255), which is always safe, and reports that. */
#include "intel_lpss_gspi.h"

#include <stddef.h>

#include "klog.h"
#include "pci.h"
#include "spi.h"
#include "vmm.h"

#define PCI_COMMAND     0x04u
#define PCI_BAR0        0x10u
#define PCI_BAR1        0x14u
#define PCI_CMD_MEMORY  0x2u

/* Virtual window for this controller's one page. e1000 uses the range below it. */
#define LPSS_MMIO_VIRT  0xFFFFA00002000000ULL
#define PTE_PCD         (1ULL << 4)

/* LPSS private block, offsets from BAR0. */
#define LPSS_PRIV_OFFSET   0x200u
#define LPSS_PRIV_RESETS   0x04u
#define LPSS_RESETS_FUNC   0x3u
#define LPSS_RESETS_IDMA   (1u << 2)

/* SSP registers, offsets from BAR0. */
#define SSCR0 0x00u
#define SSCR1 0x04u
#define SSSR  0x08u
#define SSDR  0x10u

/* SSCR0 bits. DSS holds the word size minus one: 7 means 8 bits (the usual encoding in
 * the pxa2xx driver; not re-checked against an Intel datasheet). */
#define SSCR0_DSS_8BIT 0x7u
#define SSCR0_SSE      (1u << 7)
#define SSCR0_SCR_SHIFT 8u
#define SSCR0_SCR_MASK  (0xFFu << 8)
#define SSCR0_MOD      (1u << 31)

/* SSCR1 bits: clock polarity and phase. */
#define SSCR1_SPO (1u << 3)
#define SSCR1_SPH (1u << 4)

/* SSSR bits. */
#define SSSR_TNF (1u << 2)
#define SSSR_RNE (1u << 3)
#define SSSR_BSY (1u << 4)
#define SSSR_ROR (1u << 7)

#define SPIN_LIMIT 1000000u

typedef struct {
    volatile uint8_t *regs;     /* BAR0, mapped */
    volatile uint8_t *priv;     /* regs + LPSS_PRIV_OFFSET */
    uint8_t scr;
    spi_controller_t ctrl;
    intel_lpss_gspi_info_t info;
    bool present;
} lpss_state_t;

static lpss_state_t g_lpss;

static inline uint32_t rd(uint32_t off) {
    return *(volatile uint32_t *)(g_lpss.regs + off);
}

static inline void wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_lpss.regs + off) = v;
}

static inline void wr_priv(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_lpss.priv + off) = v;
}

/* Wait until (SSSR & bit) equals want, for at most SPIN_LIMIT reads. */
static bool wait_sssr(uint32_t bit, bool want) {
    for (uint32_t i = 0; i < SPIN_LIMIT; i++) {
        bool set = (rd(SSSR) & bit) != 0;
        if (set == want) return true;
    }
    return false;
}

static void sse_off(void) {
    wr(SSCR0, rd(SSCR0) & ~SSCR0_SSE);
}

static spi_status_t lpss_set_mode(spi_controller_t *c, uint8_t cs, uint8_t mode,
                                  uint32_t speed_hz, uint8_t bits) {
    (void)c;
    (void)cs;
    (void)speed_hz;     /* the rate is fixed at SCR 255 until the input clock is verified */
    if (bits != 8u) return SPI_ERR_BAD_ARG;
    uint32_t cr1 = 0;
    if (mode & 2u) cr1 |= SSCR1_SPO;   /* CPOL */
    if (mode & 1u) cr1 |= SSCR1_SPH;   /* CPHA */
    sse_off();
    wr(SSCR1, cr1);
    return SPI_OK;
}

static spi_status_t lpss_transfer(spi_controller_t *c, uint8_t cs, const uint8_t *tx,
                                  uint8_t *rx, uint32_t len) {
    (void)c;
    (void)cs;   /* framing is the controller's; see the note at the top of this file */
    if (len == 0) return SPI_OK;
    /* SSE gates the controller on. It stays off between transfers. */
    wr(SSCR0, rd(SSCR0) | SSCR0_SSE);
    spi_status_t result = SPI_OK;
    for (uint32_t i = 0; i < len; i++) {
        if (!wait_sssr(SSSR_TNF, true)) { result = SPI_ERR_TIMEOUT; break; }
        wr(SSDR, tx != NULL ? tx[i] : 0u);
        if (!wait_sssr(SSSR_RNE, true)) { result = SPI_ERR_TIMEOUT; break; }
        uint8_t got = (uint8_t)rd(SSDR);
        if (rx != NULL) rx[i] = got;
        if ((rd(SSSR) & SSSR_ROR) != 0) { result = SPI_ERR_OVERRUN; break; }
    }
    if (result == SPI_OK && !wait_sssr(SSSR_BSY, false)) result = SPI_ERR_TIMEOUT;
    sse_off();
    return result;
}

static void fill_info(void) {
    g_lpss.info.found = g_lpss.present;
    g_lpss.info.ready = g_lpss.ctrl.ready;
    g_lpss.info.scr = g_lpss.scr;
}

bool intel_lpss_gspi_init(void) {
    if (g_lpss.ctrl.ready) return true;

    pci_function_t f;
    if (!pci_find_device((uint16_t)INTEL_LPSS_GSPI_VENDOR, (uint16_t)INTEL_LPSS_GSPI_DEVICE, &f)) {
        klog_note(KLOG_INPUT, "[spi] Intel LPSS GSPI 8086:9CE6 not found (expected on QEMU)\n");
        fill_info();
        return false;
    }
    g_lpss.present = true;
    g_lpss.info.bus = f.bus;
    g_lpss.info.device = f.device;
    g_lpss.info.function = f.function;

    /* BAR0 must be a memory BAR. Type bits 2:1: 0 is 32-bit, 2 is 64-bit (high half in BAR1). */
    uint32_t bar0 = pci_config_read32(f.bus, f.device, f.function, PCI_BAR0);
    if ((bar0 & 1u) != 0) {
        klog_note(KLOG_INPUT, "[spi] GSPI BAR0 is an I/O BAR; refusing\n");
        fill_info();
        return false;
    }
    uint64_t phys = (uint64_t)(bar0 & ~0xFu);
    uint32_t type = (bar0 >> 1) & 3u;
    if (type == 2u) {
        uint32_t bar1 = pci_config_read32(f.bus, f.device, f.function, PCI_BAR1);
        phys |= (uint64_t)bar1 << 32;
    } else if (type != 0u) {
        klog_note(KLOG_INPUT, "[spi] GSPI BAR0 has a reserved type; refusing\n");
        fill_info();
        return false;
    }
    if (phys == 0) {
        klog_note(KLOG_INPUT, "[spi] GSPI BAR0 is zero; firmware did not assign it\n");
        fill_info();
        return false;
    }
    g_lpss.info.bar_phys = phys;

    /* Memory-space decoding only. Bus mastering is not needed for programmed I/O, so we
     * do not enable it. */
    uint32_t cmd = pci_config_read32(f.bus, f.device, f.function, PCI_COMMAND);
    pci_config_write32(f.bus, f.device, f.function, PCI_COMMAND, cmd | PCI_CMD_MEMORY);

    if (!vmm_map(LPSS_MMIO_VIRT, phys & ~0xFFFULL,
                 VMM_PRESENT | VMM_WRITE | VMM_NX | PTE_PCD)) {
        klog_note(KLOG_INPUT, "[spi] could not map the GSPI registers\n");
        pci_config_write32(f.bus, f.device, f.function, PCI_COMMAND, cmd);
        fill_info();
        return false;
    }
    g_lpss.regs = (volatile uint8_t *)(uintptr_t)(LPSS_MMIO_VIRT + (phys & 0xFFFULL));
    g_lpss.priv = g_lpss.regs + LPSS_PRIV_OFFSET;

    /* Reset (intel-lpss.c: write 0, then deassert with FUNC|IDMA), then configure the SSP
     * with SSE off. */
    wr_priv(LPSS_PRIV_RESETS, 0u);
    wr_priv(LPSS_PRIV_RESETS, LPSS_RESETS_FUNC | LPSS_RESETS_IDMA);

    sse_off();
    g_lpss.scr = 255u;   /* slowest rate: always safe, see the note at the top */
    uint32_t cr0 = SSCR0_DSS_8BIT | ((uint32_t)g_lpss.scr << SSCR0_SCR_SHIFT);
    wr(SSCR0, cr0);
    wr(SSCR1, 0u);

    /* Read back: if SSCR0 does not keep what we wrote, the mapping is wrong and the
     * controller is not usable. Only then do we mark it ready. */
    uint32_t back = rd(SSCR0);
    uint32_t sssr = rd(SSSR);
    g_lpss.info.sscr0_readback = back;
    g_lpss.info.sssr = sssr;
    if ((back & ~SSCR0_SSE) != cr0 || (sssr & SSSR_TNF) == 0) {
        klog_note(KLOG_INPUT, "[spi] GSPI registers did not read back as written; not ready\n");
        fill_info();
        return false;
    }

    g_lpss.ctrl.name = "intel-lpss-gspi";
    g_lpss.ctrl.set_mode = lpss_set_mode;
    g_lpss.ctrl.transfer = lpss_transfer;
    g_lpss.ctrl.priv = &g_lpss;
    g_lpss.ctrl.ready = true;
    if (!spi_register_controller(&g_lpss.ctrl)) {
        klog_note(KLOG_INPUT, "[spi] SPI bus table is full\n");
        g_lpss.ctrl.ready = false;
    }
    klog_note(KLOG_INPUT, "[spi] Intel LPSS GSPI 8086:9CE6 ready (SCR 255, polled)\n");
    fill_info();
    return g_lpss.ctrl.ready;
}

void intel_lpss_gspi_info(intel_lpss_gspi_info_t *out) {
    fill_info();
    *out = g_lpss.info;
}
