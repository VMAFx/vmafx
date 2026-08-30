# Changelog

## [0.2.0](https://github.com/VMAFx/vmafx/compare/v0.1.0...v0.2.0) (2026-08-30)


### Features

* **mcp:** Expose tiny-AI/DNN + --feature + CTC params on score tools (ADR-1117) ([#930](https://github.com/VMAFx/vmafx/issues/930)) ([1f10e56](https://github.com/VMAFx/vmafx/commit/1f10e56d7c60d65d613652ff298c0266bd250d1a))


### Bug Fixes

* 5 real bugs from full-matrix validation — HIP wavefront / MCP probe / libvmaf.c NULL deref / ffmpeg_sycl software-frames / container YUVs ([#850](https://github.com/VMAFx/vmafx/issues/850)) ([e0e66b7](https://github.com/VMAFx/vmafx/commit/e0e66b7aa63f2e5bb5844c7169c71895c1223ac1))
* 9 critical/high round-3 bug bundle — SYCL dispatch + ADM/VIF dim guards + MCP concurrency + CUDA flush + docs ([#855](https://github.com/VMAFx/vmafx/issues/855)) ([765af26](https://github.com/VMAFx/vmafx/commit/765af26c833b5f7e2de2f6e3ad9a20f33f0edd79))
* **auth,nodes:** Constant-time session-token compare + JWT nbf validation (ADR-1021) ([#631](https://github.com/VMAFx/vmafx/issues/631)) ([76a21b4](https://github.com/VMAFx/vmafx/commit/76a21b4d0c10d66aeee578a7846f292cd33acfdc))
* **ci:** MCP ffprobe-absent mock + i686 no-asm avx512 warn + Docker ldconfig conf ([#901](https://github.com/VMAFx/vmafx/issues/901)) ([c0d58e7](https://github.com/VMAFx/vmafx/commit/c0d58e7677df89e1fdb8639eacbd88c87a00665d))
* **codeql:** CodeQL Python note-level cleanup (unused imports, lambdas, empty-except) ([#870](https://github.com/VMAFx/vmafx/issues/870)) ([a519037](https://github.com/VMAFx/vmafx/commit/a519037636c1ba8c8a93472a8b1743048b17081e))
* **codeql:** CodeQL security high/error/medium fixes (C++ + Python + CI) ([#868](https://github.com/VMAFx/vmafx/issues/868)) ([7bbaf6e](https://github.com/VMAFx/vmafx/commit/7bbaf6e85fe74645ac6bf764c9a14abb6e150f30))
* Functional-matrix failures — 17 tool/backend behaviors ([#882](https://github.com/VMAFx/vmafx/issues/882)) ([e2671ea](https://github.com/VMAFx/vmafx/commit/e2671eae489ba5418bef51fa3b0211b474780005))
* **hip,mcp:** Psnr_hip enable_chroma option + MCP Go/Python parity fixes ([#1022](https://github.com/VMAFx/vmafx/issues/1022)) ([6c0d704](https://github.com/VMAFx/vmafx/commit/6c0d704411117984e908db78c70d58b4e6621faf))
* Iter10 bundle — 8 fixes (SYCL ADM, pool condvar, UBSan/ASan, Y4M fuzz, TSan/OrtEnv, vmaf-tune, AI scripts, MCP batch) ([#875](https://github.com/VMAFx/vmafx/issues/875)) ([fea51d3](https://github.com/VMAFx/vmafx/commit/fea51d3b245236b68348c37e25181da2f3e8d434))
* Iter6 bundle — 6 fixes (wave32 comment, UBSan, TSAN, vmaf-tune CRF, AI scripts, MCP conformance) ([#865](https://github.com/VMAFx/vmafx/issues/865)) ([8b7ae73](https://github.com/VMAFx/vmafx/commit/8b7ae731a8d1e4f59b133f5e4647b99565882d37))
* Iter8 bundle — 8 fixes (GPU ADM parity, ASan, UBSan, fuzz, TSan, vmaf-tune, AI scripts, MCP) ([#871](https://github.com/VMAFx/vmafx/issues/871)) ([32ec0aa](https://github.com/VMAFx/vmafx/commit/32ec0aa6441f8d5395f57e55249c2397e405bc27))
* Iter9 bundle — 7 fixes (HIP comment, ASan .cpp, SVM fuzz, TSan race, vmaf-tune, AI scripts, MCP) ([#873](https://github.com/VMAFx/vmafx/issues/873)) ([5b7ed9b](https://github.com/VMAFx/vmafx/commit/5b7ed9b4849edc1569389fabefc52be05493a4b2))
* **licensing:** Correct SPDX id BSD-2-Clause-Patent + svm.cpp upstream copyright (ADR-1036) ([#657](https://github.com/VMAFx/vmafx/issues/657)) ([7e0cfcf](https://github.com/VMAFx/vmafx/commit/7e0cfcfa5ab3261a84a3c0f2d5f6818743921907))
* **mcp-server:** Asyncio.current_task() guard + sync ffprobe in async path + bare-except gather (ADR-1023) ([#633](https://github.com/VMAFx/vmafx/issues/633)) ([2846532](https://github.com/VMAFx/vmafx/commit/2846532c4c45ad6b4b48dabb958866abbe2223ed))
* **mcp,compat:** Round-3 bug-hunt — non-ASCII token 500, stale resource path, ffprobe KeyError ([#1059](https://github.com/VMAFx/vmafx/issues/1059)) ([007878f](https://github.com/VMAFx/vmafx/commit/007878fe7a2f9b5e2fee13bef12b90d3fb54ea11))
* **mcp:** Accept 16-bit bitdepth + drop stale vulkan backend from docs ([#919](https://github.com/VMAFx/vmafx/issues/919)) ([f9908d7](https://github.com/VMAFx/vmafx/commit/f9908d7b266729eb64676242c4ebe2c62f2bf0bd))
* **mcp:** Align precision default to C CLI legacy (%.6f) — Go + Python surfaces (ADR-1038) ([#659](https://github.com/VMAFx/vmafx/issues/659)) ([8331466](https://github.com/VMAFx/vmafx/commit/8331466699b2557cace01b02d19a4466f4ba1293))
* **mcp:** Correct chunked-body 413 propagation + null-body guard in POST /v1/score (ADR-1075) ([#779](https://github.com/VMAFx/vmafx/issues/779)) ([504a87b](https://github.com/VMAFx/vmafx/commit/504a87b8b7b5525865b7d208e84ca655a4406ed7))
* **mcp:** JSONDecodeError guards for vmaf output and ffprobe output (ADR-1010) ([#624](https://github.com/VMAFx/vmafx/issues/624)) ([98496b0](https://github.com/VMAFx/vmafx/commit/98496b0ea5555389b3ee20e2164fc708607ffd7f))
* **mcp:** Kill child processes on client disconnect — Go ctx + Python CancelledError (ADR-1085) ([#791](https://github.com/VMAFx/vmafx/issues/791)) ([658b3ea](https://github.com/VMAFx/vmafx/commit/658b3ea71f6d0b75e25d3021fd46949240531501))
* **mcp:** Port ADR-0967 HTTP hardening to Go + unify precision default + vmaf-tune stderr + eval shape guard (T-BUGHUNT-MCP-2026-06-27) ([#1046](https://github.com/VMAFx/vmafx/issues/1046)) ([6c7c3ce](https://github.com/VMAFx/vmafx/commit/6c7c3ce3319eadf6d230d8c3a7b365f7c25d9166))
* **mcp:** Probe_backend Go/Python parity — 64x64 frame + finite-score guard ([#984](https://github.com/VMAFx/vmafx/issues/984)) ([25836d6](https://github.com/VMAFx/vmafx/commit/25836d6c3b989152d017a1a262e4e9bbc6326163))
* **mcp:** Probe_backend missing-arg uses uniform required-arg ValueError ([#1033](https://github.com/VMAFx/vmafx/issues/1033)) ([a061694](https://github.com/VMAFx/vmafx/commit/a061694d31fa0e1a63449168cff044c780f68212))
* **mcp:** Restore 'missing required argument: ref' message on both servers ([#933](https://github.com/VMAFx/vmafx/issues/933)) ([b8c151c](https://github.com/VMAFx/vmafx/commit/b8c151c6961788f3c42db0e51a039383a1d06940))
* Rescue 3 crash-orphaned commits — 7 race fixes + MCP TOCTOU lock + chug-hdr state.md ([#864](https://github.com/VMAFx/vmafx/issues/864)) ([f0c4ef3](https://github.com/VMAFx/vmafx/commit/f0c4ef36c2eefcf172a865fa49c47e73d1081c59))
* **test/mcp:** Repair 11 MCP Smoke CI failures — async callsites + Vulkan removal ([#706](https://github.com/VMAFx/vmafx/issues/706)) ([cb7a8e7](https://github.com/VMAFx/vmafx/commit/cb7a8e715ae51172aa95923b19f7d03a54ee780e))


### Miscellaneous

* **deps:** Batch two months of dependency updates (supersedes 36 PRs) ([#1130](https://github.com/VMAFx/vmafx/issues/1130)) ([007e76c](https://github.com/VMAFx/vmafx/commit/007e76ccb2fe84163b34eb5f4d56712e8aed690d))
* **deps:** Bump 6 stale Python dependency floors ([#879](https://github.com/VMAFx/vmafx/issues/879)) ([4352495](https://github.com/VMAFx/vmafx/commit/4352495a57deeb50d66fd05ae6637872f30c9196))
* **deps:** Third dependency batch — typer, onnxruntime, openai, ruff ([#1147](https://github.com/VMAFx/vmafx/issues/1147)) ([4d28765](https://github.com/VMAFx/vmafx/commit/4d287651e9b7146e1ec8fd38f38eb13f8dab09f8))
* **deps:** Update dependency anyio to &gt;=4.14.0 ([#997](https://github.com/VMAFx/vmafx/issues/997)) ([ce59d3e](https://github.com/VMAFx/vmafx/commit/ce59d3efc566118d6dcf970413807343f746919f))
* **deps:** Update dependency anyio to &gt;=4.14.2 ([#1070](https://github.com/VMAFx/vmafx/issues/1070)) ([5103ac8](https://github.com/VMAFx/vmafx/commit/5103ac8f3d116e9f633670d13e99320c36ab5396))
* **deps:** Update dependency mcp to &gt;=1.28.0 ([#1018](https://github.com/VMAFx/vmafx/issues/1018)) ([d7ba3b1](https://github.com/VMAFx/vmafx/commit/d7ba3b1ef493082ca9b950a9edf97a48796a8eee))
* **deps:** Update dependency mcp to &gt;=1.28.1 [SECURITY] ([#1086](https://github.com/VMAFx/vmafx/issues/1086)) ([66b7dd3](https://github.com/VMAFx/vmafx/commit/66b7dd30def1d5b36c0d8c10a5f9de4dd442cb7b))
* **deps:** Update dependency numpy to v2.5.0 ([#1040](https://github.com/VMAFx/vmafx/issues/1040)) ([b606495](https://github.com/VMAFx/vmafx/commit/b606495bfa2c4e5f5e28fa9ff2dcd38055afa219))
* **deps:** Update dependency onnxruntime to v1.27.0 ([#998](https://github.com/VMAFx/vmafx/issues/998)) ([581dc16](https://github.com/VMAFx/vmafx/commit/581dc16b188eacb6f598c82e5a608a87d22b238b))
* **deps:** Update dependency Pillow to &gt;=12.3.0,&lt;14.0 [SECURITY] ([#1084](https://github.com/VMAFx/vmafx/issues/1084)) ([1bab26a](https://github.com/VMAFx/vmafx/commit/1bab26a8935ef821b1449e47032c55a6a289218d))
* **deps:** Update dependency pytest to &gt;=9.1.0 ([#980](https://github.com/VMAFx/vmafx/issues/980)) ([ec162d8](https://github.com/VMAFx/vmafx/commit/ec162d8032c2d5b6c6d152d86db42d5e5dcf3f93))
* **deps:** Update dependency pytest to &gt;=9.1.1 ([#1010](https://github.com/VMAFx/vmafx/issues/1010)) ([5c2957e](https://github.com/VMAFx/vmafx/commit/5c2957e1ec6f1b48ac83b6a7b6dccaf942dcfdd0))
* **deps:** Update dependency ruff to &gt;=0.15.18 ([#1011](https://github.com/VMAFx/vmafx/issues/1011)) ([a5322de](https://github.com/VMAFx/vmafx/commit/a5322deeb521687e0decb8fc0731a613b2fcaf9c))
* **deps:** Update dependency ruff to &gt;=0.16.5 ([#1165](https://github.com/VMAFx/vmafx/issues/1165)) ([6883223](https://github.com/VMAFx/vmafx/commit/68832236e91ac5d790905e798f549f3f2e7c91d8))
* **deps:** Update dependency scipy to &gt;=1.18.0 ([#1020](https://github.com/VMAFx/vmafx/issues/1020)) ([ae30576](https://github.com/VMAFx/vmafx/commit/ae3057641d7db20ae1d0f09e46cbe8357b2ae58e))
* **deps:** Update dependency torch to v2.12.1 [SECURITY] ([#1005](https://github.com/VMAFx/vmafx/issues/1005)) ([b15669a](https://github.com/VMAFx/vmafx/commit/b15669a3291252aeff5722a95d409a4e80244098))
* **deps:** Update dependency transformers to &gt;=5.11.0 ([#964](https://github.com/VMAFx/vmafx/issues/964)) ([f34eb2c](https://github.com/VMAFx/vmafx/commit/f34eb2c1e7c56e56e52b6c93b58dcdc4e3a714a4))
* **deps:** Update dependency transformers to &gt;=5.12.0 ([#992](https://github.com/VMAFx/vmafx/issues/992)) ([3591eec](https://github.com/VMAFx/vmafx/commit/3591eec3817e74bc69c49d4dd8c72b7ba98abf76))
* **deps:** Update dependency transformers to &gt;=5.12.1 ([#1000](https://github.com/VMAFx/vmafx/issues/1000)) ([d0784c9](https://github.com/VMAFx/vmafx/commit/d0784c99f56871cfe747879d0d125423f0c95614))
* **deps:** Update docker/dockerfile Docker tag to v1.25 ([#1021](https://github.com/VMAFx/vmafx/issues/1021)) ([0721f04](https://github.com/VMAFx/vmafx/commit/0721f04e50e71fa2339541946870d04cfb892822))
* **deps:** Update ubuntu:26.04 Docker digest ([#1009](https://github.com/VMAFx/vmafx/issues/1009)) ([3981316](https://github.com/VMAFx/vmafx/commit/3981316f071e873817eb0976285fc4af62264839))
* **repo:** Containerfile dpkg/git-am fixes + drop Anthropic author credit ([#924](https://github.com/VMAFx/vmafx/issues/924)) ([707f930](https://github.com/VMAFx/vmafx/commit/707f93002341d142704813aea036b9fc525a4fa0))
