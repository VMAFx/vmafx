- **Semgrep rule vmaf-no-count-store-before-bound-check.** Adds a Semgrep rule to detect
  struct count fields stored before bound checks that can reject them, preventing
  out-of-range counts on failure cleanup paths.
