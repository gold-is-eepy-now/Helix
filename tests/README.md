# tests/

Automated tests. The most important one is `boot-test.sh`.

## `boot-test.sh`

The machine-checked definition of "the kernel boots". It:

1. Boots the disk image headlessly under QEMU + UEFI (OVMF).
2. Captures the kernel's serial output.
3. Requires the three banner lines to be present.
4. Saves a screenshot of the framebuffer console.

Run it via the top-level Makefile:

```sh
make test
make BUILD_DIR=/tmp/helix-build test   # out-of-tree
```

It exits 0 only if every required line was found, so it is suitable for CI.

## Adding tests

As subsystems land, add focused tests here and wire them into `make test`.
Tests that boot the real kernel are preferred over unit tests that mock it,
because booting is the thing we most need to keep true.
