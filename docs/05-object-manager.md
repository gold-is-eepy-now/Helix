# 05 — Objects and handles (Milestone 5)

M5 adds the first native object model to the kernel. Objects have stable IDs,
types, payloads, and reference counts. A handle table gives callers small opaque
integers that refer to objects with a specific set of rights.

## Object ownership

`object_create()` returns one owned reference. The caller must eventually pass
it to `object_release()`. `object_lookup(id)` and `handle_get(...)` also return
owned references; release them when finished. Creating a handle retains the
object, and closing that handle releases its reference. The object is removed
from the global table and its destructor runs exactly once when the last
reference is released.

The normal transfer from newly created object to a handle is:

```c
object_t *event = object_create(OBJECT_TYPE_EVENT, sizeof(event_state_t), NULL);
handle_t handle = handle_create(table, event, OBJECT_RIGHT_READ |
                               OBJECT_RIGHT_WRITE | OBJECT_RIGHT_INSPECT);
object_release(event); /* the handle now owns the remaining reference */
```

Each handle carries its own rights. `handle_get()` requires at least one
requested right; a zero-right request cannot be used to obtain an object
reference. `handle_duplicate()` requires the source to have
`OBJECT_RIGHT_DUPLICATE`, and the new rights must be a subset of the source
rights. A closed slot increments its generation, so a stale handle value does
not resolve when the slot is reused. `handle_inspect()` requires the inspect
right.

The current types are event, channel, memory, process, and capability. These
are the shared type tags; implementations for the later subsystems will add
behavior to them.
The current global object table is a kernel linked list. The kernel's own
64-entry handle table is the first instance of the same table structure that
future process objects will embed.

## Synchronization boundary

Object and handle operations use single-CPU interrupt guards, which serialize
their short critical sections against PIT-driven thread preemption. This is not
an SMP lock; multicore execution will need real locks. Destruction callbacks
must not block or yield while an object is being released.

## Commands

- `objects` reports the live object count and kernel handle usage.
- `objtest` creates event and channel objects, checks lookup and type/rights
  validation, duplicates with reduced rights, rejects invalid and stale
  handles, and checks that each destructor runs on final release.

The test uses a temporary handle table and restores the global live-object
count when it finishes. The MacPorts QEMU invocation used during development
can boot the generated disk image directly:

```sh
qemu-system-x86_64 \
  -M q35 -m 512 \
  -drive if=pflash,format=raw,unit=0,readonly=on,file=/opt/local/share/qemu/edk2-x86_64-code.fd \
  -drive file=build/helix.img,format=raw,if=ide,index=0,media=disk \
  -serial stdio
```
