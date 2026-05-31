# Changelog

## [0.2.0](https://github.com/VMAFx/vmafx/compare/v0.1.0...v0.2.0) (2026-05-31)


### Features

* **mcp:** P1 surface — vmaf-tune integration, list_extractors, describe_model, progress notifications (ADR-0608) ([#1418](https://github.com/VMAFx/vmafx/issues/1418)) ([e3de80a](https://github.com/VMAFx/vmafx/commit/e3de80aef2befe208944d0fd732b8268043f1387))
* **meta:** VMAFX binary + AI tool aliases ([#1565](https://github.com/VMAFx/vmafx/issues/1565)) ([317853d](https://github.com/VMAFx/vmafx/commit/317853ded6f6cb610e397df3d8f6e351d1fa76e3))
* **server:** Vmafx-server HTTP transport + observability foundation ([#1583](https://github.com/VMAFx/vmafx/issues/1583)) ([1143a54](https://github.com/VMAFx/vmafx/commit/1143a54de9abf12c0a1e13763d96c237ced23176))


### Bug Fixes

* **ci:** 5 master CI failures — MCP smoke syntax, coverage floor, job timeouts (ADR-0637) ([#1433](https://github.com/VMAFx/vmafx/issues/1433)) ([d41757d](https://github.com/VMAFx/vmafx/commit/d41757d8ad62cfe751a248484ddafbc73f96b26f))
* **ci:** Clean libvmaf compiler warnings ([70058ee](https://github.com/VMAFx/vmafx/commit/70058ee4dea705e08f12302988cf17c0ff570dc8))
* **mcp-server:** Use NamedTemporaryFile to eliminate task-name collision risk (Round 26 A.2) ([#486](https://github.com/VMAFx/vmafx/issues/486)) ([89105b4](https://github.com/VMAFx/vmafx/commit/89105b4b5fc8820d4dd8c0677b91630ee41c623f))
* **mcp:** Extractor scanner path libvmaf/→core/ + drop deleted float_ansnr ([#242](https://github.com/VMAFx/vmafx/issues/242)) ([0e2407d](https://github.com/VMAFx/vmafx/commit/0e2407d090a3475bfd0d7e1a815ef580ac8193c9))
* **mcp:** HTTP transport — add auth + body limit + safer bind default (Round 26 audit A.1) ([#478](https://github.com/VMAFx/vmafx/issues/478)) ([af37c2a](https://github.com/VMAFx/vmafx/commit/af37c2ace828e95aed4e5ed7e0c1baa708dda9fe))
* **mcp:** IsError spec bug + probe_backend / vmaf_version / vmaf_score_encoded tools (ADR-0608) ([#1417](https://github.com/VMAFx/vmafx/issues/1417)) ([543d192](https://github.com/VMAFx/vmafx/commit/543d1929165a33bc2f8006563e89c11fed950578))
* **test:** Pytest collection crash, ADR-0543 binary path, PyTorch 2.10 deprecations ([#1559](https://github.com/VMAFx/vmafx/issues/1559)) ([993c0ef](https://github.com/VMAFx/vmafx/commit/993c0ef81112b00552d774773bd15ac6f3f772b6))
* **tests:** Resolve pre-existing failures across ai/, vmaf-tune, mcp-server ([#445](https://github.com/VMAFx/vmafx/issues/445)) ([e71c708](https://github.com/VMAFx/vmafx/commit/e71c708bab151a612d4fe46eecb6ca4b7325679b))
* **test:** Test_hip_motion3_parity skips when HIPCC kernels absent (PR [#443](https://github.com/VMAFx/vmafx/issues/443) follow-up) ([#451](https://github.com/VMAFx/vmafx/issues/451)) ([39ea00e](https://github.com/VMAFx/vmafx/commit/39ea00e4e57f0e8d0ca9362cabe8f911e5dc016e))


### Documentation

* `docs/usage/vmafx-cli.md`. ([317853d](https://github.com/VMAFx/vmafx/commit/317853ded6f6cb610e397df3d8f6e351d1fa76e3))
* **audit:** SIMD path review stub — float_adm_avx2 / vif_avx2 / motion_neon ([#116](https://github.com/VMAFx/vmafx/issues/116)) ([4a461d8](https://github.com/VMAFx/vmafx/commit/4a461d8564ae636c7e1d60cd3f7138e9c683878c))


### Miscellaneous

* **deps:** Update dependency anyio to &gt;=4.13.0 ([#1370](https://github.com/VMAFx/vmafx/issues/1370)) ([436b010](https://github.com/VMAFx/vmafx/commit/436b010266f38730f90ffcc8b07e15174ab8d16e))
* **deps:** Update dependency mcp to &gt;=1.27.1 ([#1361](https://github.com/VMAFx/vmafx/issues/1361)) ([326722b](https://github.com/VMAFx/vmafx/commit/326722b1169f7c319547ca7446dfaad4bae169a4))
* **deps:** Update dependency mypy to v2.1.0 ([#1395](https://github.com/VMAFx/vmafx/issues/1395)) ([7fb1a1d](https://github.com/VMAFx/vmafx/commit/7fb1a1dbcb1635e0a1cec0fa44276e0e8d65a2fc))
* **deps:** Update dependency numpy to v2.4.5 ([#1396](https://github.com/VMAFx/vmafx/issues/1396)) ([0db5d20](https://github.com/VMAFx/vmafx/commit/0db5d20567cdcfa92209d4a94b63fee6b809a73b))
* **deps:** Update dependency numpy to v2.4.6 ([#1442](https://github.com/VMAFx/vmafx/issues/1442)) ([a1e732d](https://github.com/VMAFx/vmafx/commit/a1e732d86277e70328ef48f9d50c545588b42aec))
* **deps:** Update dependency onnxruntime to v1.26.0 ([#1376](https://github.com/VMAFx/vmafx/issues/1376)) ([721268b](https://github.com/VMAFx/vmafx/commit/721268b9341643653e581eebe5a44642d0302d5b))
* **deps:** Update dependency openai to &gt;=2.38.0 ([#7](https://github.com/VMAFx/vmafx/issues/7)) ([cf1ba30](https://github.com/VMAFx/vmafx/commit/cf1ba303bdc0363b078a6a8166b2c50b9729e8ff))
* **deps:** Update dependency pandas to v3.0.3 ([#1398](https://github.com/VMAFx/vmafx/issues/1398)) ([fee621c](https://github.com/VMAFx/vmafx/commit/fee621c85dd691e14d11512c4369d59980e46421))
* **deps:** Update dependency pydantic to &gt;=2.13.4 ([#1381](https://github.com/VMAFx/vmafx/issues/1381)) ([2126228](https://github.com/VMAFx/vmafx/commit/21262281bf6410254e4c37dfbdff0e313508f6ef))
* **deps:** Update dependency pytest-asyncio to v1 ([#1400](https://github.com/VMAFx/vmafx/issues/1400)) ([59fc173](https://github.com/VMAFx/vmafx/commit/59fc1733dd1d755227e83483b9b63c524645ee80))
* **deps:** Update dependency ruff to &gt;=0.15.13 ([#1385](https://github.com/VMAFx/vmafx/issues/1385)) ([3fc710f](https://github.com/VMAFx/vmafx/commit/3fc710f1c1a599d797dfbbf99406c0d0bc360c47))
* **deps:** Update dependency ruff to &gt;=0.15.14 ([#1488](https://github.com/VMAFx/vmafx/issues/1488)) ([4aef033](https://github.com/VMAFx/vmafx/commit/4aef03366116e7a82dd77f1a86187cee25dbb66a))
* **deps:** Update dependency scipy to &gt;=1.17.1 ([#1388](https://github.com/VMAFx/vmafx/issues/1388)) ([34ff354](https://github.com/VMAFx/vmafx/commit/34ff354be0b1cf68eb572ada66dc555f211d90d9))
* **deps:** Update dependency torch to v2.12.0 ([#1390](https://github.com/VMAFx/vmafx/issues/1390)) ([60dbef5](https://github.com/VMAFx/vmafx/commit/60dbef5ff8c1a5065c3fd94b64a225e4571ebd28))
* **deps:** Update dependency transformers to &gt;=5.8.1 ([#1365](https://github.com/VMAFx/vmafx/issues/1365)) ([f2f5e35](https://github.com/VMAFx/vmafx/commit/f2f5e35bfb3736d59cccd11cacff7365fae08629))
* **deps:** Update dependency transformers to &gt;=5.9.0 ([#1454](https://github.com/VMAFx/vmafx/issues/1454)) ([a8dfa93](https://github.com/VMAFx/vmafx/commit/a8dfa9354c340f97df61f12f2c1e00b634ecb8d0))
* **deps:** Update docker/dockerfile Docker tag to v1.24 ([#1391](https://github.com/VMAFx/vmafx/issues/1391)) ([9aca624](https://github.com/VMAFx/vmafx/commit/9aca624d6a2b41f61cfa7824c5035dbd34a568db))
* **deps:** Update GitHub Actions (major) ([#12](https://github.com/VMAFx/vmafx/issues/12)) ([ea909b7](https://github.com/VMAFx/vmafx/commit/ea909b7116a4977742b6bdbfc230358d5161d344))
* **deps:** Update Helm release prometheus-pushgateway to v3 ([#13](https://github.com/VMAFx/vmafx/issues/13)) ([e1210d1](https://github.com/VMAFx/vmafx/commit/e1210d19d9726f85524dbb2dbfbe5d718dd41ef2))
* **deps:** Update python Docker tag to v3.14 ([#9](https://github.com/VMAFx/vmafx/issues/9)) ([e86e271](https://github.com/VMAFx/vmafx/commit/e86e2717d54f3a2fe323c424e80a6c1fd99902f2))
* **deps:** Update rocm/rocm-terminal Docker tag to v6.4 ([#10](https://github.com/VMAFx/vmafx/issues/10)) ([359b6db](https://github.com/VMAFx/vmafx/commit/359b6dbedeffab1cb8e173eea676f745982b4343))
* **meta:** Post-cutover URL sweep — lusoris/vmaf → VMAFx/vmafx ([#1](https://github.com/VMAFx/vmafx/issues/1)) ([3ec4af7](https://github.com/VMAFx/vmafx/commit/3ec4af77f0ff74fdd2e39f338b73c47ff894646e))
* **meta:** SPDX dual-license sweep across fork-added file headers ([#1548](https://github.com/VMAFx/vmafx/issues/1548)) ([d65a223](https://github.com/VMAFx/vmafx/commit/d65a22370786a0b19275bf311eeb88caaddd492b))
* **meta:** VMAFX rebrand sweep across fork-added docs and configs ([#1547](https://github.com/VMAFx/vmafx/issues/1547)) ([79b9171](https://github.com/VMAFx/vmafx/commit/79b91717e43b261db87572fd2249f7db8803a2f5))
* **python:** `__init__.py` export-completeness audit (ADR-0911) ([#402](https://github.com/VMAFx/vmafx/issues/402)) ([a341673](https://github.com/VMAFx/vmafx/commit/a341673f467caa01935fcb21fb946ddfb065f9d2))
