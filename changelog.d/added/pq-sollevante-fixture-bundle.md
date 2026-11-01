<!-- markdownlint-disable MD041 -->
- **The golden job pins all eight HDR models on real PQ with motion.** The new
  prerelease `fixtures-pq-sollevante-v1` holds an 8-frame 1080p clip derived from
  *Sol Levante* (Netflix Open Content, CC BY 4.0).
  `scripts/test/fetch-test-yuvs.sh --bundle pq-sollevante-v1` fetches it and checks
  the sha256 of the archive and of each member. A failed download fails the job,
  and `python/test/hdr_models_pin_test.py` pins its scores exactly on x86-64 and
  to 3 decimal places on aarch64 and macOS. Every CI job that runs the test fetches
  the bundle first; `scripts/ci/check_fixture_bundle_jobs.py` fails a job that does
  not.
