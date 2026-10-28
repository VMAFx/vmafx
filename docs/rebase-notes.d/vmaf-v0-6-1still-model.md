## Built-in vmaf_v0.6.1still model (2026-10-28)

- Ported from upstream PR 1391 (head `d8bac1ef0d5a`).
- `model/vmaf_v0.6.1still.json` is verbatim upstream.
- Registration in `core/src/model.c` and `core/src/meson.build`.
- When upstream merges 1391, a sync takes upstream's side; no conflict expected beyond list placement.
