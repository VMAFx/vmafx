- **The last three CI lanes moved to `ubuntu-26.04` as well.** The runner bump
  rewrote every `runs-on:` value, which left behind two matrix rows that spell
  the label as `os:` (the Intel compiler lane and the Ubuntu ARM clang lane) and
  the fallback inside the Cppcheck job's self-hosted-runner expression. All
  three now match the rest of the matrix, and the note claiming the runners were
  pinned to 24.04 because 26.04 lacked Python 3.14 is gone: the Python version
  comes from the setup action, not the image.
