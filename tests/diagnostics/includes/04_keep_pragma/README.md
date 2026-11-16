---
option: diagnostics.unused_includes
---

# Keeping an include

An include marked `IWYU pragma: keep` is never reported unused

A `// IWYU pragma: keep` comment on an include keeps it, for a header included for what including it does rather than for what it declares.
