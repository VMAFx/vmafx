- CI: the required gates that share a matrix (`Linux Intel LLVM`, `macOS Clang+Metal`,
  `Windows MSVC+CUDA (full)`, `FFmpeg Ubuntu gcc`, `FFmpeg macOS clang`) judge their own
  leg's job instead of the matrix aggregate, so one failing leg no longer turns the other
  legs' required checks red (`scripts/ci/gate_leg_result.py`).
