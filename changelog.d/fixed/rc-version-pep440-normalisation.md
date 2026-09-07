- **Release candidates no longer fail every build lane.** `setup_metadata_test.py`
  compared `setup.py --version` against `vmaf.__version__` as raw strings.
  `vmaf.__version__` carries the SemVer string release-please writes, which for
  a release candidate is a hyphenated prerelease (`1.0.0-rc.1`, ADR-1201);
  setuptools canonicalises whatever it is handed to PEP 440 before publishing
  it, so the same release reaches `setup.py --version` as `1.0.0rc1`. The two
  spellings describe one version but are not the same string, so the assertion
  failed — on `Ubuntu gcc`, `Ubuntu clang`, `Ubuntu gcc static`, `Ubuntu ARM
  clang`, the `+DNN` variants, `macOS clang`, `macOS Metal` and the FFmpeg
  lanes simultaneously, because each runs the Python suite. The comparison is
  now made on parsed versions, and a second test asserts the marker itself is
  a valid version and free of an unsubstituted `x-release-please` placeholder,
  so the check this test exists for — a marker comment leaking into the
  shipped version — is stricter than before rather than looser.
