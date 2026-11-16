import { test } from "@clice/tools/testing";

// The includes below the first declaration are recorded by the file's own
// compile, not by its precompiled preamble's build.
test(async (t) => {
    const s = await t.start();
    await t.show("main.cpp", s.diagnostics("main.cpp"), { parts: ["tags"] });
    await t.show("fixes", s.codeActions(t.marker("late"), { only: ["quickfix"] }));
});
