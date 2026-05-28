- **refactor(core):** Pilot C++20 conversion of `core/src/metadata_handler.c`
  (renamed to `.cpp` via `git mv`). `vmaf_metadata_destroy` now uses a
  `std::unique_ptr<VmafCallbackList>` with a custom `CallbackListDeleter` that
  walks and frees the linked-list nodes — replaces the manual traversal loop
  and guarantees teardown on any future early-return path. Public C API in
  `metadata_handler.h` is unchanged; `extern "C"` guards added so C callers
  (`feature_collector.c`) continue to compile and link without modification.
  Compiled as an isolated static lib (`metadata_handler_cpp20_lib`) with
  `override_options : ['cpp_std=c++20']`, matching the Vulkan VMA precedent;
  project-wide `cpp_std=c++11` default is not affected. Includes Research-0732
  migration plan and ROI-ranked candidate table for Wave 1–3 follow-up PRs.
  Netflix golden gate: `76.6678` (places=4 pass). (ADR-0708)
