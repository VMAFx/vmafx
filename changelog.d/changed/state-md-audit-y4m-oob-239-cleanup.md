- **`docs/state.md`**: audit cleanup (2026-05-05). Moved `Y4M-411-OOB`
  heap-buffer-overflow row from Open to Recently closed (PR #357 /
  commit `05ba29a6` landed the guard fix on 2026-05-04); removed the
  duplicate Y4M-OOB row + orphaned `|---|---|---|---|---|` separator
  in the Open section; removed the duplicate `#239` Vulkan-fence
  serialisation row from Open (entry already present in Recently
  closed under PR #241); cleared seven duplicate `(draft, ...)` rows
  in Recently closed whose merged-commit twins lived directly below
  them. Bumped header date to 2026-05-05. No semantic state changes —
  every closed bug stayed closed; every open bug stayed open.
