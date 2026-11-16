import { test } from "@clice/tools/testing";

// As in clangd, a header the file writes an #include of, even in a region
// the preprocessor skips, is never reported missing: the fix would include
// it twice.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
