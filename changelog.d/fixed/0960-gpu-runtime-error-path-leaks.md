fix(gpu): CUDA stream leak on `cuCtxPopCurrent` failure (A.1), picture pool
`return_to_pool` missing `pthread_cond_signal` causing deadlock (A.2), and
dangling `pic->priv` pointer after failed pool fetch (A.3). Round-25 audit.
ADR-0960.
