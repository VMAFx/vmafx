/* Copyright 2026 Lusoris; SPDX-License-Identifier: EUPL-1.2 */
/* Give horizontally scrollable code and tables keyboard access (issue 2552). */
(function () {
  function markScrollable() {
    var regions = document.querySelectorAll(
      ".md-typeset pre, .md-typeset pre > code, .md-typeset table, .md-typeset__scrollwrap, .md-sidebar__scrollwrap"
    );
    for (var i = 0; i < regions.length; i += 1) {
      var region = regions[i];
      if (!region.hasAttribute("tabindex")) {
        region.setAttribute("tabindex", "0");
      }
    }
  }

  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(markScrollable);
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", markScrollable);
  } else {
    markScrollable();
  }
  window.addEventListener("load", markScrollable);
})();
