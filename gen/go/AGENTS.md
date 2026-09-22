# gen/go — generated protobuf stubs

Everything under this directory is machine output. `buf generate` (see
`buf.gen.yaml`) writes it from `proto/`, and the `DO NOT EDIT` banner in each
`*.pb.go` means exactly that: a hand edit survives until the next regeneration
and then vanishes without a trace.

## Rebase-sensitive invariant: the HISS-09 SAFETY proofs

`protoc-gen-go` emits two raw-descriptor aliases per file —
`unsafe.Slice(unsafe.StringData(file_*_rawDesc), len(file_*_rawDesc))`, once in
`file_*_rawDescGZIP` and once in the `DescBuilder` literal of `file_*_init`.
HISS-09 requires a `// SAFETY:` proof directly above every `unsafe.*` use, and
upstream's generator has no hook for emitting one.

The proof is therefore produced by a post-generation pass, not by hand:

```bash
buf generate                                          # writes gen/go/**
python3 scripts/proto/postprocess_gen_go.py           # re-applies the proofs
python3 scripts/proto/postprocess_gen_go.py --check   # verify only
```

The pass is idempotent, so running it after every regeneration reproduces the
committed tree byte for byte. **Do not add the comments by hand, and do not
delete them to "clean up" generated output** — change
`scripts/proto/postprocess_gen_go.py` and re-run it instead.

If a future `protoc-gen-go` changes the shape of that call, the script's
`RAW_DESC_CALL` needle stops matching and `--check` still passes while the
audit starts reporting HISS-09 again. That mismatch is the signal to update
the needle, not to suppress the finding.
