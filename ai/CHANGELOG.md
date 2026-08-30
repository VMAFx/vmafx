# Changelog

## [0.2.0](https://github.com/VMAFx/vmafx/compare/v0.1.0...v0.2.0) (2026-08-30)


### Features

* **ai:** Wire run_manifest sidecar into train_konvid.py (ADR-0668 follow-up) ([#566](https://github.com/VMAFx/vmafx/issues/566)) ([d0acfba](https://github.com/VMAFx/vmafx/commit/d0acfbad556665cb7c84f547cf43e4947fefdf78))


### Bug Fixes

* **ai/tests:** Guard _load_module() against sys.modules clobber in k150k test suite ([#742](https://github.com/VMAFx/vmafx/issues/742)) ([453219e](https://github.com/VMAFx/vmafx/commit/453219ee309aa4c40bf1955429aadfacab423d04))
* **ai:** Atomic write helpers for cache and output files to prevent corruption on SIGKILL (ADR-1097) ([#821](https://github.com/VMAFx/vmafx/issues/821)) ([19b75d8](https://github.com/VMAFx/vmafx/commit/19b75d8677c2abe762be4382933bded1f2377c89))
* **ai:** Fail-loud on empty-frame clips + MOS-join key mismatch in K150K extraction ([#1028](https://github.com/VMAFx/vmafx/issues/1028)) ([54e6f14](https://github.com/VMAFx/vmafx/commit/54e6f145bd8ab9280dc1452af8e82d52965e620f))
* **ai:** Guard null/NaN data + dup keys in AI training pipeline (T-BUGHUNT-AI-2026-06-27) ([#1047](https://github.com/VMAFx/vmafx/issues/1047)) ([6d75397](https://github.com/VMAFx/vmafx/commit/6d753975d6a90a23f41fa96f010dd618bf18dda2))
* **ai:** Round-3 bug-hunt — online_trainer + extraction data-integrity ([#1057](https://github.com/VMAFx/vmafx/issues/1057)) ([038d257](https://github.com/VMAFx/vmafx/commit/038d257975d0756e7e36314e4232edc3b75db209))
* **ai:** Round-4 audit bug-fix bundle — checkpoint ghost-path, dropped-batch, degenerate scale ([#1065](https://github.com/VMAFx/vmafx/issues/1065)) ([9f96cfe](https://github.com/VMAFx/vmafx/commit/9f96cfe789371fe74e842baf9a93e3d75a7ab080))
* **codeql:** CodeQL security high/error/medium fixes (C++ + Python + CI) ([#868](https://github.com/VMAFx/vmafx/issues/868)) ([7bbaf6e](https://github.com/VMAFx/vmafx/commit/7bbaf6e85fe74645ac6bf764c9a14abb6e150f30))
* Iter10 bundle — 8 fixes (SYCL ADM, pool condvar, UBSan/ASan, Y4M fuzz, TSan/OrtEnv, vmaf-tune, AI scripts, MCP batch) ([#875](https://github.com/VMAFx/vmafx/issues/875)) ([fea51d3](https://github.com/VMAFx/vmafx/commit/fea51d3b245236b68348c37e25181da2f3e8d434))
* Iter6 bundle — 6 fixes (wave32 comment, UBSan, TSAN, vmaf-tune CRF, AI scripts, MCP conformance) ([#865](https://github.com/VMAFx/vmafx/issues/865)) ([8b7ae73](https://github.com/VMAFx/vmafx/commit/8b7ae731a8d1e4f59b133f5e4647b99565882d37))
* Iter7 bundle — 5 fixes (HIP vif parity, ASan test, fuzz key/overflow, CUDA/SYCL cleanup, AI codec-dim) ([#867](https://github.com/VMAFx/vmafx/issues/867)) ([78ed705](https://github.com/VMAFx/vmafx/commit/78ed705bcdaf90715ddef52fc57241b370f4c726))
* Iter8 bundle — 8 fixes (GPU ADM parity, ASan, UBSan, fuzz, TSan, vmaf-tune, AI scripts, MCP) ([#871](https://github.com/VMAFx/vmafx/issues/871)) ([32ec0aa](https://github.com/VMAFx/vmafx/commit/32ec0aa6441f8d5395f57e55249c2397e405bc27))
* Iter9 bundle — 7 fixes (HIP comment, ASan .cpp, SVM fuzz, TSan race, vmaf-tune, AI scripts, MCP) ([#873](https://github.com/VMAFx/vmafx/issues/873)) ([5b7ed9b](https://github.com/VMAFx/vmafx/commit/5b7ed9b4849edc1569389fabefc52be05493a4b2))
* **licensing:** Correct SPDX id BSD-2-Clause-Patent + svm.cpp upstream copyright (ADR-1036) ([#657](https://github.com/VMAFx/vmafx/issues/657)) ([7e0cfcf](https://github.com/VMAFx/vmafx/commit/7e0cfcfa5ab3261a84a3c0f2d5f6818743921907))
* **rc:** Implement picture_v2 API + fix ai/scripts stub exit codes ([#886](https://github.com/VMAFx/vmafx/issues/886)) ([d1039ac](https://github.com/VMAFx/vmafx/commit/d1039ac2cdd7422c888fd5535d106dc996cd821f))
* **testdata:** Replace hardcoded /home/kilian/dev/libvmaf_vulkan paths with env-var overrides (ADR-0792) ([#686](https://github.com/VMAFx/vmafx/issues/686)) ([44727c4](https://github.com/VMAFx/vmafx/commit/44727c49667d93b02e2f28d877d68375978a04ed))


### Tests

* **ai/scripts:** Coverage push round 2 — argv + helpers + happy paths ([#593](https://github.com/VMAFx/vmafx/issues/593)) ([2dc7eb0](https://github.com/VMAFx/vmafx/commit/2dc7eb0d8a7b24861ed3e4d16d59caf9fb5e4336))
* **ai:** Coverage push — 2 new test files lifting online_trainer + feature_extractor ([#578](https://github.com/VMAFx/vmafx/issues/578)) ([4f42702](https://github.com/VMAFx/vmafx/commit/4f4270253b38a3ce397bdd9f3e49a5f9997d7a3f))
* **ai:** Python coverage push — 70% → 82%, fix 6 pre-existing test failures ([#896](https://github.com/VMAFx/vmafx/issues/896)) ([c68228d](https://github.com/VMAFx/vmafx/commit/c68228d1aec345c0b5f67e42df6243bfcd9bce45))


### Miscellaneous

* **bundle:** Drain 8 rebased DRAFT PRs (docs + helm + tests-coverage) ([#845](https://github.com/VMAFx/vmafx/issues/845)) ([a42e4ea](https://github.com/VMAFx/vmafx/commit/a42e4ea2a456ddcf049c9520449e586981b00c28))
* **deps:** Batch two months of dependency updates (supersedes 36 PRs) ([#1130](https://github.com/VMAFx/vmafx/issues/1130)) ([007e76c](https://github.com/VMAFx/vmafx/commit/007e76ccb2fe84163b34eb5f4d56712e8aed690d))
* **deps:** Bump 6 stale Python dependency floors ([#879](https://github.com/VMAFx/vmafx/issues/879)) ([4352495](https://github.com/VMAFx/vmafx/commit/4352495a57deeb50d66fd05ae6637872f30c9196))
* **deps:** Second dependency batch — ray SECURITY, Docker digests, Actions, gomega ([#1141](https://github.com/VMAFx/vmafx/issues/1141)) ([6abe902](https://github.com/VMAFx/vmafx/commit/6abe902a3b38b90719e7f3582599cecaa5d9583a))
* **deps:** Third dependency batch — typer, onnxruntime, openai, ruff ([#1147](https://github.com/VMAFx/vmafx/issues/1147)) ([4d28765](https://github.com/VMAFx/vmafx/commit/4d287651e9b7146e1ec8fd38f38eb13f8dab09f8))
* **deps:** Update dependency matplotlib to &gt;=3.11.0 ([#957](https://github.com/VMAFx/vmafx/issues/957)) ([e074d58](https://github.com/VMAFx/vmafx/commit/e074d5871636790549b6cf340dfdaa0fcaeac079))
* **deps:** Update dependency numpy to v2.5.0 ([#1040](https://github.com/VMAFx/vmafx/issues/1040)) ([b606495](https://github.com/VMAFx/vmafx/commit/b606495bfa2c4e5f5e28fa9ff2dcd38055afa219))
* **deps:** Update dependency onnx to &gt;=1.22.0,&lt;2.0 ([#991](https://github.com/VMAFx/vmafx/issues/991)) ([721aef0](https://github.com/VMAFx/vmafx/commit/721aef0cb2da3610668ad91d57797bfd6bb2cdc6))
* **deps:** Update dependency onnxruntime to &gt;=1.27.0,&lt;2.0 ([#1001](https://github.com/VMAFx/vmafx/issues/1001)) ([fba1abd](https://github.com/VMAFx/vmafx/commit/fba1abd793eebd4f2b521df1e1c92085b06d6d1b))
* **deps:** Update dependency Pillow to &gt;=12.3.0,&lt;14.0 [SECURITY] ([#1084](https://github.com/VMAFx/vmafx/issues/1084)) ([1bab26a](https://github.com/VMAFx/vmafx/commit/1bab26a8935ef821b1449e47032c55a6a289218d))
* **deps:** Update dependency pytest to &gt;=9.1.0 ([#980](https://github.com/VMAFx/vmafx/issues/980)) ([ec162d8](https://github.com/VMAFx/vmafx/commit/ec162d8032c2d5b6c6d152d86db42d5e5dcf3f93))
* **deps:** Update dependency pytest to &gt;=9.1.1 ([#1010](https://github.com/VMAFx/vmafx/issues/1010)) ([5c2957e](https://github.com/VMAFx/vmafx/commit/5c2957e1ec6f1b48ac83b6a7b6dccaf942dcfdd0))
* **deps:** Update dependency pytest-timeout to v2 ([#972](https://github.com/VMAFx/vmafx/issues/972)) ([f6bed17](https://github.com/VMAFx/vmafx/commit/f6bed17bae81dbf24536d660e7be3465970630db))
* **deps:** Update dependency ruff to &gt;=0.15.18 ([#1011](https://github.com/VMAFx/vmafx/issues/1011)) ([a5322de](https://github.com/VMAFx/vmafx/commit/a5322deeb521687e0decb8fc0731a613b2fcaf9c))
* **deps:** Update dependency ruff to &gt;=0.16.5 ([#1165](https://github.com/VMAFx/vmafx/issues/1165)) ([6883223](https://github.com/VMAFx/vmafx/commit/68832236e91ac5d790905e798f549f3f2e7c91d8))
* **deps:** Update dependency scikit-learn to &gt;=1.9.0 ([#981](https://github.com/VMAFx/vmafx/issues/981)) ([3bcb097](https://github.com/VMAFx/vmafx/commit/3bcb0976299fe46cee70f74d86c8116c20bb2d9a))
* **deps:** Update dependency scipy to &gt;=1.18.0 ([#1020](https://github.com/VMAFx/vmafx/issues/1020)) ([ae30576](https://github.com/VMAFx/vmafx/commit/ae3057641d7db20ae1d0f09e46cbe8357b2ae58e))
* **deps:** Update dependency torch to v2.12.1 [SECURITY] ([#1005](https://github.com/VMAFx/vmafx/issues/1005)) ([b15669a](https://github.com/VMAFx/vmafx/commit/b15669a3291252aeff5722a95d409a4e80244098))
* **deps:** Update dependency torchvision to &gt;=0.27.1,&lt;0.28.0 ([#1012](https://github.com/VMAFx/vmafx/issues/1012)) ([4afad3e](https://github.com/VMAFx/vmafx/commit/4afad3eea5140df901b0e0b298360f33602cdd0a))
* **deps:** Update dependency tqdm to &gt;=4.68.3 ([#1016](https://github.com/VMAFx/vmafx/issues/1016)) ([8cfc6c4](https://github.com/VMAFx/vmafx/commit/8cfc6c40d057b95a9f9d81c461e23b14b9854be6))
* **python:** Modernize legacy typing imports in compat + ai/cli ([#721](https://github.com/VMAFx/vmafx/issues/721)) ([22317fe](https://github.com/VMAFx/vmafx/commit/22317fea90c3342abf3e732818a853d9cc2c889c))
* **repo:** Containerfile dpkg/git-am fixes + drop Anthropic author credit ([#924](https://github.com/VMAFx/vmafx/issues/924)) ([707f930](https://github.com/VMAFx/vmafx/commit/707f93002341d142704813aea036b9fc525a4fa0))
* **ruff:** Bump 0.15.17 -&gt; 0.16.5 and take the mechanical fixes ([#1157](https://github.com/VMAFx/vmafx/issues/1157)) ([3bf441a](https://github.com/VMAFx/vmafx/commit/3bf441ac1cbbce2bb1eea0d4885133ff42e48106))
