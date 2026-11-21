/*
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 *
 * Filters the hardware report table (issue #2325).
 *
 * The generator writes one row per measured backend with data-backend,
 * data-exactness, data-result, data-platform and data-search attributes, plus
 * five controls in a [data-hardware-filter] container. This script hides rows
 * that do not match every control. Without JavaScript the full table remains.
 */
(function () {
  "use strict";

  function matches(row, search, backend, exactness, result, platform) {
    var text = row.getAttribute("data-search") || "";
    for (var i = 0; i < search.length; i++) {
      if (text.indexOf(search[i]) < 0) {
        return false;
      }
    }
    return (
      (!backend || row.getAttribute("data-backend") === backend) &&
      (!exactness || row.getAttribute("data-exactness") === exactness) &&
      (!result || row.getAttribute("data-result") === result) &&
      (!platform || row.getAttribute("data-platform") === platform)
    );
  }

  function filter(panel) {
    var table = panel.parentElement.querySelector("table[data-hardware-table]");
    if (!table) {
      return;
    }
    var search = (panel.querySelector("[data-hardware-search]").value || "")
      .toLowerCase()
      .split(/\s+/)
      .filter(Boolean);
    var backend = panel.querySelector("[data-hardware-backend]").value;
    var exactness = panel.querySelector("[data-hardware-exactness]").value;
    var result = panel.querySelector("[data-hardware-result]").value;
    var platform = panel.querySelector("[data-hardware-platform]").value;
    var rows = table.querySelectorAll("tbody tr[data-hardware-row]");
    var shown = 0;
    for (var i = 0; i < rows.length; i++) {
      var visible = matches(rows[i], search, backend, exactness, result, platform);
      rows[i].hidden = !visible;
      if (visible) {
        shown++;
      }
    }
    var count = panel.querySelector("[data-hardware-count]");
    if (count) {
      count.textContent = shown + " of " + rows.length + " rows shown";
    }
    var empty = panel.parentElement.querySelector("[data-hardware-empty]");
    if (empty) {
      empty.hidden = shown !== 0;
    }
  }

  function mount(panel) {
    var controls = panel.querySelectorAll("input, select");
    for (var i = 0; i < controls.length; i++) {
      controls[i].addEventListener("input", function () {
        filter(panel);
      });
      controls[i].addEventListener("change", function () {
        filter(panel);
      });
    }
    filter(panel);
  }

  function mountAll() {
    var panels = document.querySelectorAll("[data-hardware-filter]");
    for (var i = 0; i < panels.length; i++) {
      mount(panels[i]);
    }
  }

  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(mountAll);
  } else if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", mountAll);
  } else {
    mountAll();
  }
})();
