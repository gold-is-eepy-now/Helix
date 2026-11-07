# donejs-compat

Freestanding C11 port of the DoneJS JavaScript runtime for Helix OS.

## Overview

`donejs-compat` provides an AST tree-walking JavaScript interpreter tailored for Helix OS userspace and hosted validation.

- **Standard JS Support**: Variables, functions, lexical environments, closures, objects, prototypes, arrays, exceptions, classes, template literals, destructuring, arrow functions.
- **Node-style Modules**: CommonJS `require()` resolution with built-in modules (`assert`, `buffer`, `events`, `fs`, `os`, `path`, `process`, `stream`, `util`).
- **Asynchronous Execution & Timers**: Promise implementation, microtask queues (`queueMicrotask`, `process.nextTick`), `setTimeout`, `setInterval`, and complete event loop draining.
- **Helix Freestanding Integration**: Zero libc dependencies when built with `DJS_FREESTANDING`.

## Building and Testing

Hosted test suite:
```bash
make test
```

Freestanding library:
```bash
make lib CC=x86_64-elf-gcc
```
