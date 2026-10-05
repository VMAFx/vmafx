- Split the oversized functions of the Python harness (`compat/python-vmaf/`:
  `routine.py`, `core/cross_validation.py`, `core/executor.py`,
  `tools/bd_rate.py`, `tools/testutils.py`) into private helpers so every
  function meets the HISS-04 limits (60 lines, McCabe 10, 50 statements).
  Public names, signatures, scores, output and error messages are unchanged.
