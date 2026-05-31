### Fixed: `cambi_cuda` SIGSEGV on `submit_fex_cuda` — host dereference of device pointer

`integer_cambi_cuda.c::submit_fex_cuda` called `vmaf_cambi_preprocessing(dist_pic, ...)`
directly on the CUDA picture. The CUDA picture's `data[0]` is a device pointer; reading
it on the host caused a segfault (SIGSEGV, exit code 139) on every `--feature cambi_cuda`
invocation. Fixes [Issue #857](https://github.com/VMAFx/vmafx/issues/857).

**Root cause:** `vmaf_cambi_preprocessing` internally calls
`decimate_generic_uint8_and_convert_to_10b` which dereferences `pic->data[0]` row-by-row
on the host. All other CUDA feature extractors keep preprocessing on the GPU and never
trip this; CAMBI is unique in needing a host-side decimate-and-10b-upcast.

**Fix (Option A — minimum viable):** download `dist_pic` to a transient host-pinned
`VmafPicture` via `vmaf_cuda_picture_download_async`, synchronise the picture's private
stream with `cuStreamSynchronize`, pass the host copy to `vmaf_cambi_preprocessing`, then
unref the host copy. The rest of the GPU pipeline is unchanged.

**Cross-backend parity:** the fix preserves the `places=4` contract (ADR-0214). The
decimate + 10b-upcast is the same host code used by the CPU path, so numerical output is
bit-identical. A per-frame GPU→host DtoH transfer is incurred (one luma plane at proc
resolution); the performance penalty is accepted in v1 (Option B — a GPU-native kernel
for the decimate/upcast — is deferred as a future optimisation).
