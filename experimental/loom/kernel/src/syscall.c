/* System calls from Cells. The number is in rax, arguments in rdi, rsi, rdx; the result goes back in
 * rax. Negative results are errors (the usual errno values). Every handle is checked against the
 * calling Cell's capability table, and every user pointer against its page tables. */
#include <stdint.h>
#include <string.h>
#include "arch.h"
#include "loom_cell.h"
#include "loom_kernel.h"

#define SYS_EXIT  1
#define SYS_SEND  2
#define SYS_RECV  3
#define SYS_LOG   4

#define E_PERM    (-1)
#define E_FAULT   (-14)
#define E_BADF    (-9)
#define E_AGAIN   (-11)
#define E_INVAL   (-22)
#define E_NOSYS   (-38)

#define LOG_MAX   64

struct cell_ctx *cell_current(void);

static struct loom_cell *cur(void) { return (struct loom_cell *)cell_current(); }

/* Looks up a handle and checks the rights. Returns the capability or NULL (with `err` set). */
static const struct cap *lookup(const struct loom_cell *c, uint64_t h, uint8_t need, int64_t *err) {
    if (h >= CELL_MAX_CAPS || !c->caps[h].valid) { *err = E_BADF; return NULL; }
    if (!(c->caps[h].rights & need)) { *err = E_PERM; return NULL; }
    return &c->caps[h];
}

void syscall_dispatch(struct trap_frame *f) {
    struct loom_cell *c = cur();
    int64_t ret = E_NOSYS;

    switch (f->rax) {
    case SYS_EXIT:
        cell_leave(&c->ctx, (int64_t)f->rdi);                /* does not return to the Cell */

    case SYS_SEND: {
        const struct cap *cp = lookup(c, f->rdi, CAP_SEND, &ret);
        if (!cp) break;
        struct msg m;
        m.type = (uint32_t)f->rsi;
        if (!copy_from_user(c, m.payload, f->rdx, sizeof m.payload)) { ret = E_FAULT; break; }
        ret = channel_send(cp->channel, &m);
        break;
    }

    case SYS_RECV: {
        const struct cap *cp = lookup(c, f->rdi, CAP_RECV, &ret);
        if (!cp) break;
        struct msg m;
        int r = channel_recv(cp->channel, &m);
        if (r != 0) { ret = r; break; }
        if (!copy_to_user(c, f->rsi, &m, sizeof m)) { ret = E_FAULT; break; }
        ret = 0;
        break;
    }

    case SYS_LOG: {
        uint64_t len = f->rsi;
        char buf[LOG_MAX];
        if (len > LOG_MAX || !copy_from_user(c, buf, f->rdi, (size_t)len)) { ret = E_FAULT; break; }
        serial_puts("cell: ");
        for (uint64_t i = 0; i < len; i++) serial_putc(buf[i]);
        serial_puts("\n");
        ret = (int64_t)len;
        break;
    }

    default:
        break;
    }
    (void)E_INVAL;
    f->rax = (uint64_t)ret;
}
