### Chore

- Promote `docker-image.yml` Docker Image Build job from advisory to blocking:
  remove `continue-on-error: true` (met criterion: 3 consecutive green master
  runs). Add a CPU score-assertion smoke step that runs `vmaf --backend cpu`
  inside the built image against the 576x324 YUV fixture pair in `testdata/`
  and verifies mean VMAF ≈ 94.32 ± 0.5 (model `vmaf_v0.6.1.json`), proving
  the binary executes correctly rather than merely that the Dockerfile parses.
  Timeout raised from 30 min to 45 min to accommodate the score computation.
