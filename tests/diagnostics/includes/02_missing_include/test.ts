import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("before", s.diagnostics("main.cpp"));
    await t.show("fixes", s.codeActions(t.marker("missing"), { only: ["quickfix"] }));
    await t.show("after", s.applyAction(t.marker("missing"), "insert '#include \"point.h\"'"));
});
