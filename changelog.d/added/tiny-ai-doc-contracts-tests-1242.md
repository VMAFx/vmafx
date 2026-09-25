- **Tiny-AI runtime contracts and doc audit (#1242):** audited and resolved
  three runtime contract and documentation gaps across `docs/ai/`: documented
  the unimplemented status of sidecar checkpoint quarantine and controller
  stability gating in `docs/ai/sidecar-online-training.md` (while noting atomic
  ONNX export and `.sha256` sidecar generation are implemented), documented the
  100-frame sliding-window contract of shipped `core/src/feature/transnet_v2.c`
  in `docs/ai/extractor-template.md`, and documented that inference cross-device
  parity bounds in `docs/ai/inference.md` are workstation measurements rather
  than CI-gated runs. Added regression contract test suites in
  `ai/tests/test_tiny_ai_doc_contracts.py` and
  `ai/sidecar/tests/test_quickstart_contract.py` (Research-2110).
