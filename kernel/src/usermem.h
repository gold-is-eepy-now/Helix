/* usermem.h — checked access to user memory from a system call.
 *
 * A system call receives pointers from ring 3. The kernel must never trust
 * them: a pointer can name unmapped memory, kernel memory, or a page the
 * process is not allowed to write. Every user buffer goes through one of these
 * checks before the kernel reads or writes it. The checks walk the page tables
 * of the current address space, which is the calling process's address space
 * while a system call runs. */
#ifndef HELIX_USERMEM_H
#define HELIX_USERMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True if every byte in [addr, addr + len) is mapped user memory, and writable
 * when `write` is set. A zero-length range is always valid. */
bool usermem_valid(uint64_t addr, uint64_t len, bool write);

/* Copy `len` bytes from user memory into `dst` (kernel). False if the range is
 * not valid user memory. */
bool usermem_copy_in(void *dst, uint64_t user_src, uint64_t len);

/* Copy `len` bytes from kernel `src` into user memory. False if the range is not
 * valid user memory. */
bool usermem_copy_out(uint64_t user_dst, const void *src, uint64_t len);

/* Copy a NUL-terminated string of at most cap-1 bytes into `dst` (always
 * terminated). False if no terminator is found within the limit or the bytes
 * are not valid user memory. */
bool usermem_copy_str(char *dst, size_t cap, uint64_t user_src);

#endif /* HELIX_USERMEM_H */
