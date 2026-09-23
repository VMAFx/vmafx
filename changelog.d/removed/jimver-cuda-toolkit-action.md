- Remove the `Jimver/cuda-toolkit` action from the Linux CUDA build legs, and
  with it `renovate.json`'s `cuda:` matchString, which matched nothing once the
  action was gone. A manager whose pattern matches nothing looks wired and does
  nothing. See ADR-1300.
