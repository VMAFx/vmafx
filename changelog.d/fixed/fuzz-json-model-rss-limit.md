Tighten nightly fuzzer RSS limit for `fuzz_json_model` to 512 MB (down from the
default 2048 MB) to catch the runaway 1.1 GB peak RSS observed in 180-second runs.

Fix partial-parse leak in the `fuzz_json_model` harness: `model_c` and `collection`
are now freed unconditionally after
`vmaf_read_json_model_collection_from_buffer`, eliminating the residual leak that
could accumulate when a collection parse succeeded on early keys then failed on a
later one. Re-enable ASan leak detection (`detect_leaks=1`) now that the root cause
is addressed.
