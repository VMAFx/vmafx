# Vulkan integer ADM — committed parity evidence (ADR-3402)

`refs/` — bit-exact references produced by the **unmodified CPU**
`integer_adm` extractor at public master `af5ec5c94` through the public
API (`cpu_ref.c` pattern: `adm` feature with `debug=true`, `%a`
hex-float contract, 48 frames x 11 fields per fixture):

- `cpu-adm-576x324-48f.txt` — fixture `testdata/{ref,dis}_576x324_48f.yuv`
- `cpu-adm-640x480-48f.txt` — fixture `testdata/{ref,dis}_640x480_48f.yuv`

`out/` — the GPU-side outputs of the fork PoC gate
(`poc/vk-integer-adm` @ `0bb98ee21`, 2026-11-06), committed so the
comparison can be re-checked without a GPU (`compare.py`-equivalent in
`test_vulkan_integer_adm_parity.c`):

- `vk-adm-576x324-48f.txt` — RTX 5080 (`0x10de:0x2c02`), driver 621.257
- `vk-adm-576x324-48f-4060.txt` — RTX 4060 Laptop (`0x10de:0x28e0`)
- `vk-adm-640x480-48f.txt` — RTX 5080

The PoC outputs were produced by the phase-1 shader that kept the
Netflix-style scale-0 CM own-tap int16 wrap AND the fp64 angle test.
The in-tree shader (ADR-3402 deviations 2 and 3) drops the wrap
(ADR-1402; both CPU anchors agree on these fixtures, so the refs are
unchanged by the drop) and evaluates the angle predicate through the
fp64-free `adm_angle_flag_i64()` reformulation (bit-identical for every
int64 triple, pinned by `test_adm_angle_flag.c`). **The committed
`out/` files therefore predate the two in-tree shader upgrades; the
parity gate must be re-run on real hardware before any merge decision
(maintainers' own runner, including a planted flip the gate must
refuse — #3302 comment 6691339657, #3319 work item 2).** The refs need
no re-run: they are CPU-side and both CPU anchors produce them
byte-identically.

sha256 of the gate fixtures (upstream `testdata/`, yuv420p8):

```
ref_576x324_48f.yuv  1a61ff158ab0dacdd3c64e4c63602a7c2ef6da10cba0d8aa7893f6ae9fb64816
dis_576x324_48f.yuv  4e9a22d7edc1fd8c1aac396366a9eaaf1f946bec834c18bcfb2713e70bce8f26
ref_640x480_48f.yuv  2430b0de1aad6d44f2680b13a1c3a32ba4ec9ec9506ca65c5fed8199b909537d
dis_640x480_48f.yuv  984cbae373b70dade18077bb8a72aeda2257bc9663ca55f924833ecfa870976d
```
