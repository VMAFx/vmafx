- **The CLI option parsers leaked their option-string buffer on every
  error path.** `parse_model_config()` and `parse_feature_config()`
  `malloc` a working copy of the option string and hand ownership to
  the returned config, which `cli_free()` releases once it reaches
  `CLISettings`. On a parse error they called `usage()` before that
  handover, so the buffer — and, for features, the partially built
  `VmafFeatureDictionary` — became unreachable. Invisible in the
  shipped CLI, where `usage()` is `_Noreturn` and ends the process, but
  the libFuzzer harness intercepts `exit` via `-Wl,--wrap=exit` and
  longjmps back into its own frame, so the leak is real there: it made
  the nightly `fuzz_cli_parse` leg fail intermittently, and only
  intermittently, because LeakSanitizer reports the block just when its
  conservative scan no longer sees the stale pointer. Both functions
  now release the buffer and the dictionary before reporting the error.
  The error strings point *into* that buffer, so they are copied first
  — freeing before formatting printed freed memory and turned
  `bad option string "bogusflag"` into `bad option string ""`.
