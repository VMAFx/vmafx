- **refactor(predict,libvmaf)**: extract bootstrap score-name suffix constants
  and `BOOTSTRAP_NAME_BUF_SZ()` macro into shared `bootstrap_names.h`; remove
  two `//TODO: dedupe` markers that referenced each other (ADR-0480).
