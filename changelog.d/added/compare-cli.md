- **`vmaf-tune compare` CLI subcommand
  ([T-VMAF-TUNE-compare-codecs](T-VMAF-TUNE-compare-codecs.md)).**
  Surfaces the existing `compare.py` codec-comparison ranker as a
  runnable subcommand. Takes a comma-separated `--encoders` list,
  delegates to [`compare_codecs`](../tools/vmaf-tune/src/vmaftune/compare.py),
  ranks by smallest bitrate at the chosen `--target-vmaf`, emits a
  markdown / JSON / CSV report. Module shipped earlier with its tests;
  this entry just exposes it on the CLI.
