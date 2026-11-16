import { test } from "@clice/tools/testing";

// An edit below the leading includes keeps the precompiled preamble: the
// includes it holds are still checked against what the file now uses.
test(async (t) => {
    const s = await t.start();
    await t.show("before", s.diagnostics("main.cpp"));
    s.edit("main.cpp", { replace: "int ticks = elapsed();", with: "int ticks = now();" });
    await t.show("after", s.diagnostics("main.cpp"));
});
