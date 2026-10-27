- CI: the gates that judge their own matrix leg read the run's jobs again while their leg still
  reads unfinished, instead of failing at once. The jobs API had reported the finished
  `FFmpeg Ubuntu gcc work` leg without a conclusion for a few seconds and failed master
  `baeb82e6f` (`T-CI-GATE-JOBS-API-LAG-2026-10-27`).
