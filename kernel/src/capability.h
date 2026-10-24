/* capability.h — revocable, rights-limited authority over kernel objects. */
#ifndef HELIX_CAPABILITY_H
#define HELIX_CAPABILITY_H

#include <stdbool.h>
#include <stdint.h>

#include "object.h"

/* Create authority from a source handle that has INSPECT and DUPLICATE.
 * Requested rights must be a non-empty subset of that handle's rights. */
handle_t capability_create(handle_table_t *table, handle_t source,
                           uint32_t granted_rights);

/* Derivation can only reduce the parent's rights. All descendants share the
 * parent's revocation domain. Derived handles cannot revoke that domain. */
handle_t capability_derive(handle_table_t *table, handle_t source,
                           uint32_t reduced_rights);

/* A capability handle with CONTROL may revoke its whole derivation tree. */
bool capability_revoke(handle_table_t *table, handle_t capability);

/* Returns a retained target object when the requested authority is present. */
object_t *capability_resolve(handle_table_t *table, handle_t capability,
                             uint32_t required_rights,
                             object_type_t expected_type);

bool capability_is_revoked(handle_table_t *table, handle_t capability);

/* Trusted kernel code can mint a CONTROL capability over a page-aligned
 * virtual-memory range. The guarded operation below can change permissions
 * only for pages inside that range.
 */
handle_t capability_create_memory_region(handle_table_t *table,
                                         uint64_t base, uint64_t size);
bool capability_protect_memory(handle_table_t *table, handle_t capability,
                               uint64_t virtual_address, uint64_t flags);

#endif /* HELIX_CAPABILITY_H */
