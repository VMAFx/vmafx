- Isolate FFmpeg replay/smoke, dependency-classifier and agent-cleanup test
  fixtures from inherited Git repository, index and configuration variables.
  Local and CI regression checks now verify that disposable caller repositories
  retain their configuration, refs, object store, index and working files.
