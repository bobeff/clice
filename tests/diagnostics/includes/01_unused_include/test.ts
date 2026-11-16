import { test } from "@clice/tools/testing";

test(async (t) => {
    const s = await t.start();
    await t.show("before", s.diagnostics("main.cpp"), { parts: ["tags"] });
    await t.show("fixes", s.codeActions(t.marker("unused"), { only: ["quickfix"] }));
    await t.show("after", s.applyAction(t.marker("unused"), "remove '#include \"clock.h\"'"));
});
