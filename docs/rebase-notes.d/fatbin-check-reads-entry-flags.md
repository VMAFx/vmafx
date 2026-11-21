## Fatbin compression entry flags (2026-11-20)

- `core/src/check_device_compression.py` reads fatbin entry flags and excludes
  compressed payloads from raw-code scans. Preserve this check when updating
  the CUDA build integration.
