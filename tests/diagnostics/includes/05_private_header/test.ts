import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"));
    await t.show("fixes", s.codeActions(t.marker("missing"), { only: ["quickfix"] }));
});
