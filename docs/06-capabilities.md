# 06 — Capabilities (Milestone 6)

Capabilities are revocable grants of specific rights over an object. A
capability handle refers to a capability object; its payload retains the target
object, the granted rights, and a shared revocation domain.

## Current API

- `capability_create(table, source, rights)` creates a grant when the source
  handle has `INSPECT` and `DUPLICATE`, and the requested rights are a non-empty
  subset of that handle's rights.
- `capability_derive(table, source, reduced_rights)` creates a child grant
  whose rights are a subset of the parent. The child shares the parent's
  revocation domain.
- `capability_resolve(table, cap, required, type)` returns a retained target
  only if the capability has not been revoked and covers the requested rights
  and type. The caller must `object_release()` the returned object.
- `capability_revoke(table, cap)` requires `CONTROL` on the capability handle
  and revokes that grant and every descendant. Derived handles do not receive
  `CONTROL` by default.

These APIs establish the authority model, but existing kernel services have
not all migrated from direct object handles to capabilities yet. Channel
transfer is sequenced with M8 because channels do not provide message delivery
yet. The current implementation inherits M5's single-CPU, kernel-context-only
synchronization assumption.

## Self-test

`captest` checks capability creation, reduced-rights derivation, rejection of
rights enlargement, type/rights checks, denial of child revocation, subtree
revocation, and release of all objects after the test.
