/* input.h — the kernel's unified character input source. */
#ifndef HELIX_INPUT_H
#define HELIX_INPUT_H

/* Poll every input device and return one character, or -1 if none is ready.
 * Never blocks. Serial is checked first so automated tests can drive the
 * shell; the PS/2 keyboard serves the graphical console. */
int input_getchar(void);

#endif /* HELIX_INPUT_H */
