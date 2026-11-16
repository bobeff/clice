---
option: diagnostics.missing_includes
---

# Private headers

A header marked private is included through the header it names

When the header declaring what the file uses says `// IWYU pragma: private, include "…"`, the quick fix includes the header it names instead.
