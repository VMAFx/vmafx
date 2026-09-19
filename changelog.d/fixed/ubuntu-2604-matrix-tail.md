- **The last three CI lanes moved to `ubuntu-26.04` as well.** The runner bump
  rewrote every `runs-on:` value, which left behind two matrix rows that spell
  the label as `os:` (the Intel compiler lane and the Ubuntu ARM clang lane) and
  the fallback inside the Cppcheck job's self-hosted-runner expression. All
  three now match the rest of the matrix, and the note claiming the runners were
  pinned to 24.04 because 26.04 lacked Python 3.14 is gone: the Python version
  comes from the setup action, not the image.
- **The `Docs` job stopped being cancelled at its own timeout.** The strict
  documentation build was already taking over nine minutes against a ten-minute
  ceiling, and on the newer runner image it crosses it, so the job died
  mid-build and the required-checks aggregator reported a failure that had
  nothing to do with the documentation. The ceiling is twenty minutes now.
  The `Cppcheck` lane stays on the older image for the moment: its analyser
  comes from the image, and the newer one reports ninety findings in the
  vendored libsvm predictor which are real, in scope, and deserve a fix with a
  golden-data run rather than a suppression.
