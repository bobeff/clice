import { test } from "@clice/tools/testing";

// A header that compiles alone is checked as a source is.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("widget.h", s.diagnostics("widget.h"));
});
