---
option: diagnostics.missing_includes
---

# Missing includes

`missing_includes = true` points at what the file uses without including it

A name the file uses from a header it reaches only through another include is pointed out, with a quick fix that includes that header after the file's leading includes.
