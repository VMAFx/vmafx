- The backend receipt of the JSON report (`backend_used`, `feature_backends`)
  is written by the library's report writer instead of being spliced into the
  file by the `vmaf` CLI, so reports written through the API carry it too;
  `feature_backends` now comes before `backend_used` in the file. The
  `provenance` object of WP8 keeps its six members and gains the full record
  (#2142, ADR-2073).
