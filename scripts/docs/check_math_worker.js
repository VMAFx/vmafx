// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// Compiles every formula that scripts/docs/check_math.py hands over with the
// vendored KaTeX (the same file the site serves), in strict mode with
// throwOnError. Input on stdin: a JSON array of {"tex": string, "display":
// boolean}. Output on stdout: a JSON array of {"index": number, "message":
// string}, one entry per formula that does not compile.
"use strict";

const path = require("path");
const katex = require(
  path.join(__dirname, "..", "..", "docs", "javascripts", "vendor", "katex", "katex.min.js")
);

const chunks = [];
process.stdin.on("data", (chunk) => chunks.push(chunk));
process.stdin.on("end", () => {
  const formulas = JSON.parse(Buffer.concat(chunks).toString("utf8"));
  const failures = [];
  formulas.forEach((formula, index) => {
    try {
      katex.renderToString(formula.tex, {
        displayMode: Boolean(formula.display),
        throwOnError: true,
        strict: "error",
        trust: false,
      });
    } catch (err) {
      failures.push({ index, message: String(err && err.message ? err.message : err) });
    }
  });
  process.stdout.write(JSON.stringify(failures));
});
