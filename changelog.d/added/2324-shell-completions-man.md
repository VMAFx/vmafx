- CLI: shell completions for bash, zsh, fish and PowerShell and a man page render
  from the option groups of the API definition (`core/api/vmafx.toml`), covering
  both installed command names (`vmaf` and the `vmafx` symlink), every long
  spelling with its aliases and underscore spellings, the short options, the
  value switches and the argument domain of every option whose values the
  definition lists (`scripts/codegen/vmafx_api/emit_completions.py`; the files
  are generated outputs and the generator's `--check` refuses a stale one).
