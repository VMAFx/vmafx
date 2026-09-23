- **The exported-symbol gate no longer fails on a compiler-generated variant of a
  public function.** icpx emits `<name>|_._.<n>._.<m>` beside a function it clones
  for offload; the checker compared the decorated name against the header name set
  and flagged `vmaf_dnn_session_run|_._.1._.1` as a leaked symbol, although its
  parent carries `VMAF_EXPORT`. The suffix is now stripped before the lookup, so a
  variant is accepted exactly when its parent is public — and still rejected when
  it is not.
