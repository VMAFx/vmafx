## dev image: ClangBuildAnalyzer built with Ninja (2026-10-17)

- The `build-deps` stage of `dev/Containerfile` installs `ninja-build` and no make program, so the ClangBuildAnalyzer configure passes `-G Ninja`. A rebase that moves or rewrites that step keeps the generator, or adds a make program to the stage's package list.
- `scripts/ci/tests/test_dev_image_build_stages.py` (`test_cmake_in_a_stage_without_make_names_a_generator`) refuses a CMake configure without `-G` in `build-deps`.
