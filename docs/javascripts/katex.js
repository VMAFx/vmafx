/*
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Typesets the formulas of a page (ADR-2705).
 *
 * pymdownx.arithmatex (generic mode) wraps each `$...$` and `$$...$$` of the
 * Markdown in an element with class "arithmatex" holding `\(...\)` or `\[...\]`.
 * This script renders those with the vendored KaTeX
 * (docs/javascripts/vendor/katex/) on every page load and after Material's
 * instant navigation (`document$`). It follows Material's documented KaTeX
 * recipe, except that only the `\(` and `\[` delimiters are scanned: a stray
 * dollar sign in prose is never read as math, and only the arithmatex elements
 * are searched. Without JavaScript the TeX source stays readable.
 */
(function () {
  "use strict";

  function typeset(root) {
    if (typeof renderMathInElement !== "function") {
      return;
    }
    var nodes = root.querySelectorAll(".arithmatex");
    for (var i = 0; i < nodes.length; i++) {
      renderMathInElement(nodes[i], {
        delimiters: [
          { left: "\\[", right: "\\]", display: true },
          { left: "\\(", right: "\\)", display: false }
        ],
        throwOnError: false,
        strict: "ignore"
      });
    }
  }

  if (typeof document$ !== "undefined") {
    document$.subscribe(function () {
      typeset(document.body);
    });
  } else {
    document.addEventListener("DOMContentLoaded", function () {
      typeset(document.body);
    });
  }
})();
