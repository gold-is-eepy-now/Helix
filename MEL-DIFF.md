# MEL-DIFF — MelDev vs Dev

This file tracks how the **MelDev** branch differs from **Dev**, and keeps a
dated journal of changes made on MelDev. Newest entries are at the top of the
journal.

> **Status:** The Dev-vs-MelDev comparison below is **pending**. It needs a
> full branch comparison, which has not been run yet. The journal is complete
> for the changes listed.

---

## Differences from Dev

| Area | Dev | MelDev |
|------|-----|--------|
| Version identity | `0.5.18-dev.20261110` | `0.5.19-dev.20261114.MELDEV` |
| Build date | 2026-11-10 | 2026-11-14 |

_Full list of functional differences: pending branch comparison._

---

## Journal

### 2026-11-14

| Time (UTC) | Change | Commit | Notes |
|------------|--------|--------|-------|
| 23:54 | Bumped version to `0.5.19-dev.20261114.MELDEV` in `kernel/src/version.h` | [`602f512`](https://github.com/mymel2001-holder/Helix-MelDev/commit/602f512d651446abf63e75517b2e4b4dc9d74292) | Version string only. **Not yet built or booted** (`make clean && make test && make iso` still to run, per CLAUDE.md). |
| 23:5x | Created this file (`MEL-DIFF.md`) | this commit | Journal started. |
