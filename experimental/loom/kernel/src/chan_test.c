/* Two-Cell test: typed messages over a channel, enforced by capabilities, between Cells that do not
 * share memory. Cell A sends; Cell B receives. A also tries three forbidden operations and reads a
 * page that belongs to B only. Results are read back from the Cells' own pages after they run. */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom_cell.h"
#include "loom_kernel.h"
#include <assert.h>

#define DATA_VA    0x500000ull
#define SECRET_VA  0x600000ull
#define STACK_VA   0x700000ull
#define FAULT14    (0x1000 + 14)

extern const uint8_t cell_a_start[], cell_a_end[];
extern const uint8_t cell_b_start[], cell_b_end[];

static void report(const char *what, bool ok) {
    serial_puts(ok ? "PASS: " : "FAIL: ");
    serial_puts(what);
    serial_puts("\n");
}

static int64_t word(const uint8_t *p) { int64_t v; memcpy(&v, p, 8); return v; }

/* Runs one Cell with a code image, a data page (copied to `out` before the page is freed), an
 * optional secret page (B's own), and the given capability table. Returns the exit status. */
static int64_t run_cell(const uint8_t *start, const uint8_t *end, const struct cap caps[CELL_MAX_CAPS],
                        bool with_secret, const char *data_init, uint8_t out[256]) {
    uint64_t space = vm_new_space();
    uint64_t code = pmm_alloc_frame(), data = pmm_alloc_frame(), stack = pmm_alloc_frame();
    uint64_t secret = with_secret ? pmm_alloc_frame() : 0;
    assert(space && code && data && stack);

    memset((void *)phys_to_virt(data), 0, 4096);
    memset((void *)phys_to_virt(stack), 0, 4096);
    memcpy((void *)phys_to_virt(code), start, (size_t)(end - start));
    if (data_init) memcpy((void *)phys_to_virt(data), data_init, strlen(data_init) + 1);
    if (with_secret) {
        memset((void *)phys_to_virt(secret), 0, 4096);
        memcpy((void *)phys_to_virt(secret), "SECRET-B", 9);
    }

    bool ok = vm_map_user(space, 0x400000ull, code, PTE_USER_RX) &&
              vm_map_user(space, DATA_VA, data, PTE_USER_RW) &&
              vm_map_user(space, STACK_VA, stack, PTE_USER_RW) &&
              (!with_secret || vm_map_user(space, SECRET_VA, secret, PTE_USER_RW));
    assert(ok);

    struct loom_cell c;
    memset(&c, 0, sizeof c);
    c.ctx.user_cr3 = space;
    c.ctx.entry = 0x400000ull;
    c.ctx.user_rsp = STACK_VA + 4096 - 16;
    memcpy(c.caps, caps, sizeof c.caps);

    int64_t status = cell_run(&c.ctx);
    memcpy(out, (void *)phys_to_virt(data), 256);   /* read the results before the page is freed */
    vm_free_space(space);                            /* frees code, data, stack and secret frames */
    return status;
}

bool chan_selftest(void) {
    bool all = true;
    uint64_t free0 = pmm_free_count();

    int chan = channel_create(1);
    assert(chan >= 0);

    /* Cell A: handle 0 = SEND, handle 1 = RECV only, on the same channel. */
    struct cap caps_a[CELL_MAX_CAPS] = {0};
    caps_a[0] = (struct cap){ .valid = true, .object = CAP_OBJ_CHANNEL, .rights = CAP_SEND, .index = (uint8_t)chan };
    caps_a[1] = (struct cap){ .valid = true, .object = CAP_OBJ_CHANNEL, .rights = CAP_RECV, .index = (uint8_t)chan };
    uint8_t out_a[256];
    int64_t st_a = run_cell(cell_a_start, cell_a_end, caps_a, false, "hello from A", out_a);

    bool a_killed = st_a == FAULT14;
    report("cell A is killed when it reads memory that is not its own", a_killed); all &= a_killed;
    bool a_send_ok = word(out_a + 0x80) == 0;
    report("A's send on its SEND handle with the declared type succeeds", a_send_ok); all &= a_send_ok;
    bool a_wrong_type = word(out_a + 0x88) == -22;
    report("A's send with the wrong type is refused (EINVAL)", a_wrong_type); all &= a_wrong_type;
    bool a_no_right = word(out_a + 0x90) == -1;
    report("A's send through a RECV-only handle is refused (EPERM)", a_no_right); all &= a_no_right;

    /* Cell B: handle 0 = RECV on the same channel. It holds no SEND handle. It has its own secret. */
    struct cap caps_b[CELL_MAX_CAPS] = {0};
    caps_b[0] = (struct cap){ .valid = true, .object = CAP_OBJ_CHANNEL, .rights = CAP_RECV, .index = (uint8_t)chan };
    uint8_t out_b[256];
    int64_t st_b = run_cell(cell_b_start, cell_b_end, caps_b, true, NULL, out_b);

    bool b_exit = st_b == 0;
    report("cell B receives and exits normally (status 0)", b_exit); all &= b_exit;
    bool recv_rc = word(out_b + 0x98) == 0;
    report("B's receive on its RECV handle succeeds", recv_rc); all &= recv_rc;
    uint32_t got_type;
    memcpy(&got_type, out_b + 0x40, 4);
    bool type_ok = got_type == 1;
    report("B receives the declared type unchanged", type_ok); all &= type_ok;
    bool payload_ok = memcmp(out_b + 0x44, "hello from A", 12) == 0;
    report("B receives A's payload byte for byte", payload_ok); all &= payload_ok;
    bool no_handle = word(out_b + 0xa0) == -9;
    report("B's send on a handle it does not hold is refused (EBADF)", no_handle); all &= no_handle;

    bool no_leak = pmm_free_count() == free0;
    report("the channel test leaves no frames behind", no_leak); all &= no_leak;
    return all;
}
