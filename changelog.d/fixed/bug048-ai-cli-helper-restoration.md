- Restored the shared import bootstrap, argument-parser construction, and
  replay-argv handling across twelve tiny-AI evaluation, quantization, and
  export scripts after a later training-scaffold merge silently reverted them.
  Direct file and module invocation now follow the same CLI contract.
