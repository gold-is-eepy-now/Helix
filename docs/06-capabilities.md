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
- `capability_allows(table, cap, required, type)` is the unified authority
  gate used by kernel operations: it reports whether a capability is live,
  unrevoked, and covers the required rights and type, without retaining the
  target. Kernel operations should call this before acting on a resolved
  target.
- `capability_revoke(table, cap)` requires `CONTROL` on the capability handle
  and revokes that grant and every descendant. Derived handles do not receive
  `CONTROL` by default.

Capability creation rejects zero granted rights. Derivation can only reduce
rights, and derivation from a revoked parent fails. Channel transfer is
sequenced with M8 because channels do not provide message delivery yet. The
current implementation inherits M5's single-CPU, kernel-context-only
synchronization assumption.

## Self-test

`captest` checks capability creation, reduced-rights derivation, rejection of
rights enlargement and zero granted rights, type/rights checks through
`capability_allows`, denial of child revocation, subtree revocation, rejection
of derivation from a revoked parent, rejection of missing capability handles, and
release of all objects after the test.
