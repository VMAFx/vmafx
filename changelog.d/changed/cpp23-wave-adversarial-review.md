### chore(review): adversarial code review — C++23 wave (PRs #41–#58)

Read-only adversarial review of the cpp23 conversion wave. Found 4 CRITICAL, 2 HIGH,
10 MEDIUM, 3 LOW issues across all 9 PRs. Key findings:
- `strtof` precision bug in `dict.cpp` causing score corruption on high-precision options (#48)
- `strlen - 5U` unsigned underflow heap-overflow in `model.cpp` (#54)
- `operator new` / `free()` allocator mismatch in `ref.cpp` (#58)
- Non-NUL-terminated `string_view::data()` passed to `strtol` in `opt.cpp` (#43)
See `docs/research/cpp23-wave-adversarial-review-20260528.md` for the full findings table.
