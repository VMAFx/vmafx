- `adm_dwt2_8_neon` now matches the scalar `adm_dwt2_8` bit-for-bit. Two
  divergences had gone undetected because **no unit test covered this kernel on
  any architecture** — the same blind spot that let ADR-1057's dropped filter
  tap reach master and drift the ARM golden.
  - **Stale intermediate columns for widths ≡ 8 (mod 16).** The dispatcher in
    `integer_adm.c` admits the NEON kernel on `!(w % 8)`, but its vertical pass
    advances 16 columns at a time and stops at `w - 15`, with no scalar tail. For
    a width such as 584 or 776 the final 8 columns of `tmplo` / `tmphi` were
    never written in that iteration, so the horizontal pass consumed whatever the
    previous row had left in the shared `buf->tmp_ref` scratch.
  - **Unmirrored final column.** The horizontal pass walks `tmplo` / `tmphi` with
    plain pointer arithmetic and never consults `ind_x`, so for the last output
    column it read `tmplo[w]` — which is `tmphi[0]`, since `tmphi == tmplo + w` —
    where the scalar kernel mirrors the index back to `w - 1`.
- **Neither changed a VMAF score** on any input tested, including a synthetic
  584-wide clip built specifically to trigger the first one: ADM crops the
  affected border columns before they reach a feature value, and pre-fix,
  post-fix and x86-scalar all produced `80.322171`. This is a correctness and
  cross-backend-parity fix, not a scoring fix.
- New `core/test/test_adm_dwt2_neon.c` asserts bit-exactness against a scalar
  reference across six geometries chosen to exercise both `w % 16 == 0` and
  `w % 16 == 8`, plus odd heights. It reproduced both defects before the fix
  (24×16: 160 mismatches, of which 128 were the stale columns) and passes after.
