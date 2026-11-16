import { test } from "@clice/tools/testing";

// Both checks are off unless clice.toml turns them on.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
});
