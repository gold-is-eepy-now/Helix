/* helix.h — minimal C interface for native Helix user programs. */
#ifndef HELIX_USER_HELIX_H
#define HELIX_USER_HELIX_H

typedef unsigned long helix_size_t;

long helix_write(const void *buffer, helix_size_t length);
long helix_read(void *buffer, helix_size_t capacity);
long helix_clear(void);
__attribute__((noreturn)) void helix_exit(int status);

#endif /* HELIX_USER_HELIX_H */
