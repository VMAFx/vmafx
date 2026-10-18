- **Imported CUDA frames are measured to be scored in place.** On an RTX
  4090, frames handed to `vmafx_frame_import()` score bit for bit as host
  uploads on every exact CUDA twin, a skipped or early fence fails the fence
  test, and Nsight Systems shows no memory copy on the import path. The
  device-to-device luma copies six CUDA twins make for every frame, imported
  or uploaded, are tracked in `docs/state.md` for the tuning phase.
