<!-- markdownlint-disable MD013 MD033 MD060 -->
<!-- REUSE-IgnoreStart -->
# Credits and acknowledgements

VMAFx stands on other people's work. This page names every third-party item the
project ships, vendors, adapts, learns from or uses, says how it reaches the
project, and states the licence the upstream gives it. If you want to know who
to thank, what a vendored file's terms are, or where a model's weights came
from, start here.

The licence texts of the code that ships are in
[`LICENSES/`](https://github.com/VMAFx/vmafx/tree/master/LICENSES); the terms
per file are in [`REUSE.toml`](https://github.com/VMAFx/vmafx/blob/master/REUSE.toml)
and explained in [Licensing](licensing.md). VMAFx itself is a fork of
[Netflix VMAF](https://github.com/Netflix/vmaf); the history is in
[the references](reference/references.md).

## At a glance

<!-- credits:table summary -->
226 entries. By kind: upstream 2, code 22, library 67, tool 45, action 33, image 7, font 2, model 7, dataset 11, paper 15, standard 7, text 8. By relation: shipped 44, vendored 20, adapted 22, inspired 13, used-by-CI 72, integrated 55.

30 entries have a licence this page could not verify upstream: [Bampis, Gupta, Soundararajan and Bovik, SpEED-QA (IEEE SPL 2017)](https://doi.org/10.1109/lsp.2017.2726542), [Contributor Covenant 2.1](https://www.contributor-covenant.org/version/2/1/code_of_conduct/), [Debian base image](https://hub.docker.com/_/debian), [Developer Certificate of Origin 1.1](https://developercertificate.org/), [Documenting Architecture Decisions (Michael Nygard)](https://www.cognitect.com/blog/2011/11/15/documenting-architecture-decisions), [Doxygen](https://github.com/doxygen/doxygen), [Go base image](https://hub.docker.com/_/golang), [Hadolint](https://github.com/hadolint/hadolint), [Intel oneAPI Base Toolkit](https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit.html), [Intel oneAPI Base Toolkit image](https://hub.docker.com/r/intel/oneapi-basekit), [ITU-R BT.2022, general viewing conditions for subjective assessment](https://www.itu.int/rec/R-REC-BT.2022), [ITU-R BT.2100, image parameter values for HDR television](https://www.itu.int/rec/R-REC-BT.2100), [ITU-R BT.2124-0 (2019), potential visibility of colour differences in television](https://www.itu.int/rec/R-REC-BT.2124), [Li, Zhang, Ma and Ngan, Image quality assessment by separately evaluating detail losses and additive impairments (IEEE TMM 2011)](https://doi.org/10.1109/tmm.2011.2152382), [Mantiuk and Azimi, PU21 (QoMEX and PCS 2021)](https://ieeexplore.ieee.org/document/9477471/), [MATLAB reference implementations (iCID, SpEED, STMAD, matlabPyrTools, colorspace)](https://github.com/Netflix/vmaf), [matplotlib (Python)](https://github.com/matplotlib/matplotlib), [Mittal, Moorthy and Bovik, No-reference image quality assessment in the spatial domain (IEEE TIP 2012)](https://doi.org/10.1109/tip.2012.2214050), [Mittal, Soundararajan and Bovik, Making a completely blind image quality analyzer (IEEE SPL 2013)](https://doi.org/10.1109/lsp.2012.2227726), [NASA/JPL Power of 10 and the JPL C coding standard](https://spinroot.com/gerard/pdf/P10.pdf), [Python base image](https://hub.docker.com/_/python), [QEMU](https://github.com/qemu/qemu), [ROCm development image](https://hub.docker.com/r/rocm/dev-ubuntu-26.04), [SEI CERT C Coding Standard](https://wiki.sei.cmu.edu/confluence/display/c), [Sheikh and Bovik, Image information and visual quality (IEEE TIP 2006)](https://doi.org/10.1109/tip.2005.859378), [SMPTE ST 2084:2014, Perceptual Quantizer EOTF](https://doi.org/10.5594/SMPTE.ST2084.2014), [Swatinem/rust-cache](https://github.com/Swatinem/rust-cache), [Toward a better quality metric for the video community (Netflix, 2020)](https://netflixtechblog.com/toward-a-better-quality-metric-for-the-video-community-7ed94e752a30), [Toward a practical perceptual video quality metric (Netflix, 2016)](https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652), [Ubuntu base image](https://hub.docker.com/_/ubuntu).
<!-- credits:end -->

## How to read the tables

Each row is one item. The **relation** says how much of it reaches the project:

| Relation | Meaning |
| --- | --- |
| shipped | Its code or data is in the release binaries, images or archives. |
| vendored | Upstream files are reproduced in this repository under their own licence. |
| adapted | Upstream code, rules or structure are rewritten for VMAFx, or an implementation follows a published method. |
| inspired | Only the idea is taken; nothing is copied or shipped. |
| used-by-CI | It runs in the build, the hooks or CI and ships nowhere. |
| integrated | VMAFx reads, writes or calls it, or needs it installed to build or run a feature. |

The **licence** is an SPDX expression exactly as the upstream states it, or one
of three words: `proprietary` for a closed product, a service or custom terms
used as their provider states them (the note says which), `none` when the
upstream states no licence, and `unknown` when it could not be verified. An
`unknown` is a gap to close, not a clearance: do not rely on it. GitHub-hosted
licences were read from each repository on 2026-10-08.

## Upstream projects and vendored or adapted code

Code that came from somewhere else. Files that carry a third-party copyright
header keep the upstream terms (ADR-1250); the paths column lists them.

<!-- credits:table code -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [av-metrics (CIEDE2000)](https://github.com/rust-av/av-metrics) | adapted | `MIT` | `core/src/feature/ciede.c`, `core/src/feature/ciede_ff_math.h`, `core/src/feature/cuda/integer_ciede/ciede_device.h`, `core/src/feature/cuda/integer_ciede/ciede_score.cu`, and 11 more | Joshua Holmer; the CIEDE2000 maths of the ciede extractor and its twins. |
| [cJSON](https://github.com/DaveGamble/cJSON) | vendored | `MIT` | `core/src/mcp/3rdparty/cJSON/` | Vendored for the embedded MCP server. |
| [Daala video input code](https://github.com/xiph/daala) | vendored | `BSD-2-Clause` | `core/tools/vidinput.c`, `core/tools/vidinput.h`, `core/tools/y4m_input.c` | Y4M and video readers of the CLI. |
| [dav1d CPU detection and atomics shim](https://code.videolan.org/videolan/dav1d) | vendored | `BSD-2-Clause` | `core/src/compat/gcc/stdatomic.h`, `core/src/x86/cpu.c`, `core/src/x86/cpuid.asm`, `tools/rc1-tester/image/published-rc/scans/v1.0.0-rc.1/cpu.json`, and 7 more | VideoLAN and dav1d authors, Two Orioles, LLC. |
| [FFmpeg](https://github.com/FFmpeg/FFmpeg) | integrated | `LGPL-2.1-or-later AND GPL-2.0-or-later` | `ffmpeg-patches/` | The patch series adds libvmaf filters; patch 0019 also touches GPL units. Upstream states most files LGPL v2.1 or later and some GPL. |
| [HACL*](https://github.com/hacl-star/hacl-star) | shipped | `MIT` | `tools/rc1-tester/image/licenses/hacl-star.txt` | Linked into the interpreter of the tester artifacts; the licence text is reproduced for their notices (ADR-1503). |
| [interfig (from hindsight)](https://github.com/vectorize-io/hindsight) | vendored | `MIT` | `tools/figures/third_party/interfig/` | Figure engine vendored for interactive figures (ADR-0015). |
| [IQA by Tom Distler](https://tdistler.com/) | vendored | `BSD-3-Clause` | `core/src/feature/arm64/convolve_neon.c`, `core/src/feature/arm64/convolve_neon.h`, `core/src/feature/arm64/ms_ssim_decimate_neon.c`, `core/src/feature/arm64/ms_ssim_decimate_neon.h`, and 62 more | Image-quality library behind the SSIM and MS-SSIM extractors; SIMD and GPU twins keep its header. |
| [Kubernetes OpenAPI v3 specification](https://github.com/kubernetes/kubernetes) | vendored | `Apache-2.0` | `deploy/helm/vmafx/values.schema.json`, `deploy/helm/vmafx/THIRD-PARTY-NOTICES.txt`, `api/kubernetes/openapi-subset.json`, `scripts/codegen/k8s_openapi.py`, and 1 more | Type schemas of the Helm chart's values schema, from the release build-config.env pins (K8S_SCHEMA_VERSION); attributed in the chart's THIRD-PARTY-NOTICES.txt (ADR-2350 D13, ADR-2673). |
| [libjxl SSIMULACRA 2 reference](https://github.com/libjxl/libjxl) | adapted | `BSD-3-Clause` | `core/src/feature/arm64/ssimulacra2_arm64_common.h`, `core/src/feature/arm64/ssimulacra2_host_neon.c`, `core/src/feature/arm64/ssimulacra2_host_neon.h`, `core/src/feature/arm64/ssimulacra2_neon.c`, and 29 more | The JPEG XL Project Authors tools/ssimulacra2.cc is the reference the SSIMULACRA 2 port follows. |
| [libsvm](https://github.com/cjlin1/libsvm) | vendored | `BSD-3-Clause` | `core/src/svm.cpp`, `core/src/svm.h` | Chih-Chung Chang and Chih-Jen Lin; SVM model evaluation for VMAF models. |
| [LIME local explainer](https://github.com/marcotcr/lime) | vendored | `BSD-2-Clause` | `compat/python-vmaf/core/local_explainer.py` | Marco Tulio Correia Ribeiro, 2016. |
| [MATLAB reference implementations (iCID, SpEED, STMAD, matlabPyrTools, colorspace)](https://github.com/Netflix/vmaf) | vendored | `unknown` | `compat/python-vmaf/matlab/` | Inherited from Netflix/vmaf; most files state no licence. iCID is supplementary material of Preiss, Fernandes and Urban (IEEE TIP 2014); the colorspace code is by Pascal Getreuer. |
| [mkdirp.c by Stephen Mathieson](https://github.com/stephenmathieson/mkdirp.c) | adapted | `MIT` | `core/src/feature/mkdirp.cpp`, `core/src/feature/mkdirp.h` | C++ conversion of the original C implementation. |
| [Netflix VMAF](https://github.com/Netflix/vmaf) | vendored | `BSD-2-Clause-Patent AND BSD-3-Clause-Clear` (the inherited Python harness files are tagged BSD-3-Clause-Clear) | `NOTICE`, `core/`, `compat/python-vmaf/`, `python/`, and 2 more | The project VMAFx is forked from. Inherited files keep Netflix terms (ADR-1250). |
| [nv-codec-headers](https://git.ffmpeg.org/gitweb/ffmpeg/nv-codec-headers.git) | shipped | `MIT` | `tools/rc1-tester/image/licenses/nv-codec-headers.txt` | Notice reproduced for the CUDA images published before ADR-1513 (ADR-1578). |
| [NVIDIA's CUDA backend in Netflix VMAF](https://github.com/Netflix/vmaf) | vendored | `BSD-2-Clause-Patent` | `core/src/cuda/common.c`, `core/src/cuda/common.h`, `core/src/cuda/cuda_helper.cuh`, `core/src/cuda/picture_cuda.c`, and 32 more | Files that carry NVIDIA Corporation copyright, from the CUDA backend NVIDIA contributed to Netflix/vmaf. |
| [pdjson](https://github.com/skeeto/pdjson) | vendored | `Unlicense` | `core/src/pdjson.c`, `core/src/pdjson.h` | Streaming JSON parser for model files. |
| [pelorus](https://github.com/VMAFx/pelorus) | vendored | `EUPL-1.2` | `core/include/libvmaf/pelorus/`, `core/src/interop/`, `core/test/test_pelorus_interop.c` | Interop ABI headers, parser and conformance fixture vendored from the VMAFx/pelorus repository. |
| [PU21 reference implementation (gfxdisp/pu21)](https://github.com/gfxdisp/pu21) | adapted | `BSD-3-Clause` | `docs/metrics/pu21.md` | Encoder coefficients, formula and range for the pu21 extractor. |
| [scanf.py by Danny Yoo](https://github.com/Netflix/vmaf) | vendored | `BSD-2-Clause` | `compat/python-vmaf/tools/scanf.py` | Copied into Netflix/vmaf; the file header names the author and carries the SPDX tag. |
| [STRRED MATLAB code (LIVE, University of Texas at Austin)](https://live.ece.utexas.edu/research/quality/) | vendored | `proprietary` (header: educational and research use without fee; not to be used as the basis of a commercial product without permission) | `compat/python-vmaf/matlab/strred/strred/` | Inherited from Netflix/vmaf. |
| [x264 x86inc.asm](https://code.videolan.org/videolan/x264) | vendored | `ISC` | `core/src/ext/x86/x86inc.asm` | x264 project assembler macros. |
| [Xiph.Org Daala code (PSNR-HVS and helpers)](https://github.com/xiph/daala) | vendored | `BSD-2-Clause` | `core/src/feature/arm64/psnr_hvs_neon.c`, `core/src/feature/arm64/psnr_hvs_neon.h`, `core/src/feature/cuda/float_ssim_cuda.h`, `core/src/feature/cuda/integer_psnr_hvs/psnr_hvs_score.cu`, and 45 more | Xiph.Org code behind the PSNR-HVS extractor and its twins. |
<!-- credits:end -->

## Libraries

Dependencies that are compiled into the binaries (Go modules) or imported by the
Python tools. Only direct dependencies are listed; the release images carry the
full notices ([Licensing](licensing.md)).

<!-- credits:table libraries -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [aiohttp (Python)](https://github.com/aio-libs/aiohttp) | integrated | `Apache-2.0 AND MIT` | `mcp-server/vmaf-mcp/pyproject.toml` |  |
| [anyio (Python)](https://github.com/agronholm/anyio) | integrated | `MIT` | `mcp-server/vmaf-mcp/pyproject.toml` |  |
| [api (Go)](https://github.com/kubernetes/api) | shipped | `Apache-2.0` | `go.mod` | Go module k8s.io/api. |
| [apimachinery (Go)](https://github.com/kubernetes/apimachinery) | shipped | `Apache-2.0` | `go.mod` | Go module k8s.io/apimachinery. |
| [chi (Go)](https://github.com/go-chi/chi) | shipped | `MIT` | `go.mod` | Go module github.com/go-chi/chi. |
| [cilium/ebpf](https://github.com/cilium/ebpf) | shipped | `MIT` | `cmd/vmafx-node/bpf/`, `go.mod` | Loads and generates the rclone-bypass eBPF object of the node daemon. |
| [client-go (Go)](https://github.com/kubernetes/client-go) | shipped | `Apache-2.0` | `go.mod` | Go module k8s.io/client-go. |
| [client_golang (Go)](https://github.com/prometheus/client_golang) | shipped | `Apache-2.0` | `go.mod` | Go module github.com/prometheus/client_golang. |
| [client_model (Go)](https://github.com/prometheus/client_model) | shipped | `Apache-2.0` | `go.mod` | Go module github.com/prometheus/client_model. |
| [cobra (Go)](https://github.com/spf13/cobra) | shipped | `Apache-2.0` | `go.mod` | Go module github.com/spf13/cobra. |
| [controller-runtime (Go)](https://github.com/kubernetes-sigs/controller-runtime) | shipped | `Apache-2.0` | `go.mod` | Go module sigs.k8s.io/controller-runtime. |
| [D3](https://github.com/d3/d3) | shipped | `ISC` | `docs/javascripts/vendor/vega/` | Reached through the Vega bundle. |
| [defusedxml (Python)](https://github.com/tiran/defusedxml) | integrated | `PSF-2.0` | `python/pyproject.toml` |  |
| [dill (Python)](https://github.com/uqfoundation/dill) | integrated | `BSD-3-Clause` | `python/pyproject.toml` |  |
| [fx (Go)](https://github.com/uber-go/fx) | shipped | `MIT` | `go.mod` | Go module go.uber.org/fx. |
| [ginkgo (Go)](https://github.com/onsi/ginkgo) | shipped | `MIT` | `go.mod` | Go module github.com/onsi/ginkgo. |
| [golusoris](https://github.com/golusoris/golusoris) | shipped | `EUPL-1.2` | `go.mod` | Go framework of the services. |
| [gomega (Go)](https://github.com/onsi/gomega) | shipped | `MIT` | `go.mod` | Go module github.com/onsi/gomega. |
| [goptuna (Go)](https://github.com/c-bata/goptuna) | shipped | `MIT` | `go.mod` | Go module github.com/c-bata/goptuna. |
| [grafana-foundation-sdk (Go)](https://github.com/grafana/grafana-foundation-sdk) | shipped | `Apache-2.0` | `go.mod` | Go module github.com/grafana/grafana-foundation-sdk. |
| [grpc-go (Go)](https://github.com/grpc/grpc-go) | shipped | `Apache-2.0` | `go.mod` | Go module google.golang.org/grpc. |
| [h5py (Python)](https://github.com/h5py/h5py) | integrated | `BSD-3-Clause` | `python/pyproject.toml` |  |
| [joblib (Python)](https://github.com/joblib/joblib) | integrated | `BSD-3-Clause` | `python/pyproject.toml` |  |
| [jsonschema (Python)](https://github.com/python-jsonschema/jsonschema) | integrated | `MIT` | `ai/pyproject.toml`, `tools/rc1-tester/pyproject.toml` |  |
| [KaTeX](https://github.com/KaTeX/KaTeX) | shipped | `MIT` | `docs/javascripts/vendor/katex/` | Formula renderer of the documentation site (ADR-2705); also compiles every formula in the docs build check. |
| [kin-openapi (Go)](https://github.com/getkin/kin-openapi) | shipped | `MIT` | `go.mod` | Go module github.com/getkin/kin-openapi. |
| [libsvm-official (Python)](https://github.com/cjlin1/libsvm) | integrated | `BSD-3-Clause` | `python/pyproject.toml` |  |
| [logr (Go)](https://github.com/go-logr/logr) | shipped | `Apache-2.0` | `go.mod` | Go module github.com/go-logr/logr. |
| [matplotlib (Python)](https://github.com/matplotlib/matplotlib) | integrated | `unknown` | `ai/pyproject.toml`, `python/pyproject.toml`, `tools/ensemble-training-kit/pyproject.toml`, `tools/vmaf-tune/pyproject.toml` |  |
| [mcp (Python)](https://github.com/modelcontextprotocol/python-sdk) | integrated | `MIT` | `mcp-server/vmaf-mcp/pyproject.toml` |  |
| [MCP Go SDK](https://github.com/modelcontextprotocol/go-sdk) | shipped | `Apache-2.0 AND MIT` (upstream is moving from MIT to Apache-2.0; both apply) | `go.mod` | Go module github.com/modelcontextprotocol/go-sdk. |
| [modernc.org/sqlite](https://gitlab.com/cznic/sqlite) | shipped | `BSD-3-Clause` (from its LICENSE file; the repository is not on GitHub) | `go.mod` | Pure Go SQLite driver. |
| [numpy (Python)](https://github.com/numpy/numpy) | integrated | `BSD-3-Clause AND 0BSD AND MIT AND Zlib AND CC0-1.0` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml`, `python/pyproject.toml`, and 2 more |  |
| [onnx (Python)](https://github.com/onnx/onnx) | integrated | `Apache-2.0` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml`, `tools/ensemble-training-kit/pyproject.toml`, and 1 more |  |
| [onnxruntime (Python)](https://github.com/microsoft/onnxruntime) | integrated | `MIT` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml`, `tools/ensemble-training-kit/pyproject.toml`, and 2 more |  |
| [onnxscript (Python)](https://github.com/microsoft/onnxscript) | integrated | `MIT` | `ai/pyproject.toml` |  |
| [opentelemetry-go (Go)](https://github.com/open-telemetry/opentelemetry-go) | shipped | `Apache-2.0` | `go.mod` | Go module go.opentelemetry.io/otel. |
| [opentelemetry-go-contrib (Go)](https://github.com/open-telemetry/opentelemetry-go-contrib) | shipped | `Apache-2.0` | `go.mod` | Go module go.opentelemetry.io/contrib. |
| [optuna (Python)](https://github.com/optuna/optuna) | integrated | `MIT` | `ai/pyproject.toml`, `tools/ensemble-training-kit/pyproject.toml`, `tools/vmaf-tune/pyproject.toml` |  |
| [pandas (Python)](https://github.com/pandas-dev/pandas) | integrated | `BSD-3-Clause` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml`, `python/pyproject.toml`, and 1 more |  |
| [parquet-go (Go)](https://github.com/parquet-go/parquet-go) | shipped | `Apache-2.0` | `go.mod` | Go module github.com/parquet-go/parquet-go. |
| [pflag (Go)](https://github.com/spf13/pflag) | shipped | `BSD-3-Clause` | `go.mod` | Go module github.com/spf13/pflag. |
| [Prometheus Pushgateway Helm chart](https://github.com/prometheus-community/helm-charts) | vendored | `Apache-2.0` | `deploy/helm/vmafx/charts/` | Chart dependency fetched by helm dependency build. |
| [prometheus-client (Python)](https://github.com/prometheus/client_python) | integrated | `Apache-2.0 AND BSD-2-Clause` | `mcp-server/vmaf-mcp/pyproject.toml` |  |
| [protobuf-go (Go)](https://github.com/protocolbuffers/protobuf-go) | shipped | `BSD-3-Clause` | `go.mod` | Go module google.golang.org/protobuf. |
| [pyarrow (Python)](https://github.com/apache/arrow) | integrated | `Apache-2.0` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml` |  |
| [pydantic (Python)](https://github.com/pydantic/pydantic) | integrated | `MIT` | `ai/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml` |  |
| [python-slugify (Python)](https://github.com/un33k/python-slugify) | integrated | `MIT` | `python/pyproject.toml` |  |
| [pytorch-lightning (Python)](https://github.com/Lightning-AI/pytorch-lightning) | integrated | `Apache-2.0` | `ai/pyproject.toml` |  |
| [PyWavelets (Python)](https://github.com/PyWavelets/pywt) | integrated | `MIT AND BSD-3-Clause` | `python/pyproject.toml` |  |
| [pyyaml (Python)](https://github.com/yaml/pyyaml) | integrated | `MIT` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `tools/ensemble-training-kit/pyproject.toml` |  |
| [React, react-dom and scheduler (Meta)](https://github.com/facebook/react) | shipped | `MIT` | `tools/figures/dist/` | Bundled in the figure player; texts in dist/THIRD-PARTY-LICENSES.txt. |
| [rich (Python)](https://github.com/Textualize/rich) | integrated | `MIT` | `ai/pyproject.toml`, `dev-llm/pyproject.toml` |  |
| [scikit-image (Python)](https://github.com/scikit-image/scikit-image) | integrated | `BSD-3-Clause` | `python/pyproject.toml` |  |
| [scikit-learn (Python)](https://github.com/scikit-learn/scikit-learn) | integrated | `BSD-3-Clause` | `ai/pyproject.toml`, `python/pyproject.toml`, `tools/ensemble-training-kit/pyproject.toml` |  |
| [scipy (Python)](https://github.com/scipy/scipy) | integrated | `BSD-3-Clause` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml`, `python/pyproject.toml`, and 1 more |  |
| [seaborn (Python)](https://github.com/mwaskom/seaborn) | integrated | `BSD-3-Clause` | `ai/pyproject.toml` |  |
| [sureal (Python)](https://github.com/Netflix/sureal) | integrated | `Apache-2.0` | `python/pyproject.toml` |  |
| [sync (Go)](https://github.com/golang/sync) | shipped | `BSD-3-Clause` | `go.mod` | Go module golang.org/x/sync. |
| [torch (Python)](https://github.com/pytorch/pytorch) | integrated | `Apache-2.0 AND Apache-2.0 WITH LLVM-exception AND BSD-2-Clause AND BSD-3-Clause AND BSL-1.0 AND MIT` | `ai/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml`, `tools/ensemble-training-kit/pyproject.toml`, `tools/vmaf-tune/pyproject.toml` |  |
| [torchvision](https://github.com/pytorch/vision) | integrated | `BSD-3-Clause` | `ai/lpips_export.py`, `ai/pyproject.toml` | SqueezeNet 1.1 features under LPIPS. |
| [tqdm (Python)](https://github.com/tqdm/tqdm) | integrated | `MPL-2.0 AND MIT` | `ai/pyproject.toml` |  |
| [typer (Python)](https://github.com/fastapi/typer) | integrated | `MIT` | `ai/pyproject.toml`, `dev-llm/pyproject.toml`, `mcp-server/vmaf-mcp/pyproject.toml` |  |
| [uuid (Go)](https://github.com/google/uuid) | shipped | `BSD-3-Clause` | `go.mod` | Go module github.com/google/uuid. |
| [Vega](https://github.com/vega/vega) | shipped | `BSD-3-Clause` | `docs/javascripts/vendor/vega/` | Chart runtime in the documentation; the bundle lists its dependencies in THIRD-PARTY-LICENSES.txt. |
| [vega-embed](https://github.com/vega/vega-embed) | shipped | `BSD-3-Clause` | `docs/javascripts/vendor/vega/` |  |
| [Vega-Lite](https://github.com/vega/vega-lite) | shipped | `BSD-3-Clause` | `docs/javascripts/vendor/vega/` |  |
<!-- credits:end -->

## Models

<!-- credits:table models -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [BRISQUE model (LIVE, University of Texas at Austin)](https://live.ece.utexas.edu/research/quality/) | shipped | `LicenseRef-LIVE-BRISQUE` | `model/other_models/brisque_live.model`, `model/other_models/NOTICE-brisque` | Used under the release notice as written; publications reporting research with it acknowledge and cite it (ADR-1507). |
| [DISTS (Ding et al.)](https://github.com/dingkeyan93/DISTS) | inspired | `MIT` | `docs/metrics/dists.md`, `docs/ai/models/dists_sq.md` | The shipped checkpoint is a smoke placeholder, not the authors production weights. |
| [FastDVDnet weights (m-tassano/fastdvdnet)](https://github.com/m-tassano/fastdvdnet) | shipped | `MIT` | `model/tiny/fastdvdnet_pre.*`, `ai/scripts/export_fastdvdnet_pre.py` | Matias Tassano; the temporal pre-filter model. |
| [LPIPS (richzhang/PerceptualSimilarity)](https://github.com/richzhang/PerceptualSimilarity) | shipped | `BSD-2-Clause AND BSD-3-Clause` | `model/tiny/lpips_sq.*`, `ai/lpips_export.py` | LPIPS linear layers on torchvision SqueezeNet 1.1 ImageNet features. |
| [MobileSal](https://github.com/yuhuan-wu/MobileSal) | inspired | `CC-BY-NC-SA-4.0` | `docs/ai/models/mobilesal.md` | Not shipped: the upstream weights are incompatible with the project licence; the shipped placeholder and the fork-trained students are not derived from them. |
| [TransNet V2 weights (soCzech/TransNetV2)](https://github.com/soCzech/TransNetV2) | shipped | `MIT` | `model/tiny/transnet_v2.*`, `ai/scripts/export_transnet_v2.py` | Tomas Soucek; shot-boundary model. |
| [U-2-Net (u2netp)](https://github.com/xuebinqin/U-2-Net) | integrated | `LicenseRef-Apache-2.0-u2netp` | `docs/ai/u2netp-mirror.md`, `docs/ai/models/u2netp_mirror_card.md` | Checkpoint mirrored as a release asset, not committed; the upstream weights are Apache-2.0. |
<!-- credits:end -->

## Datasets

Training and evaluation data. Several datasets forbid redistribution or
commercial use; VMAFx ships no dataset file. The terms are quoted from the
dataset pages in [Training data](ai/training-data.md#dataset-terms).

<!-- credits:table datasets -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [BVI-DVC](https://fan-aaron-zhang.github.io/BVI-DVC/) | integrated | `proprietary` (all rights stay with the originators of each sequence; one source is for academic research only) | `ai/scripts/bvi_dvc_to_corpus_jsonl.py`, `docs/ai/bvi-dvc-corpus-ingestion.md` | Ma, Zhang and Bull. Not redistributed. |
| [CHUG UGC-HDR](https://doi.org/10.1109/ICIP55913.2025.11084488) | integrated | `CC-BY-NC-SA-4.0` (the README badge says CC BY-NC 4.0 while license.txt holds the NC-SA text) | `ai/scripts/chug_to_corpus_jsonl.py`, `docs/ai/chug-ingestion.md` | Saini, Bovik, Birkbeck, Wang and Adsumilli, ICIP 2025. |
| [DUTS-TR](https://saliencydetection.net/duts/) | integrated | `proprietary` (all rights reserved by the original authors; images come from ImageNet) | `docs/ai/models/saliency_student_v1.md` | Training input of the saliency students; no file is redistributed. |
| [ImageNet](https://image-net.org/download.php) | integrated | `proprietary` (non-commercial research and education only) | `docs/ai/models/saliency_student_v1.md` | Source of the DUTS-TR images. |
| [KonViD-150k](https://database.mmsp-kn.de/konvid-150k-vqa-database.html) | integrated | `proprietary` (research-only per ADR-0325) | `ai/scripts/konvid_150k_to_corpus_jsonl.py`, `docs/ai/konvid-150k-ingestion.md` | Not redistributed. |
| [KoNViD-1k](https://database.mmsp-kn.de/konvid-1k-database.html) | integrated | `none` (the page names no licence and offers the database to the research community) | `ai/scripts/konvid_1k_to_corpus_jsonl.py`, `docs/ai/konvid-1k-ingestion.md` | Not redistributed. Clips keep the Creative Commons licence of their YFCC100M upload. |
| [LIVE-VQC](https://live.ece.utexas.edu/research/LIVEVQC/) | integrated | `proprietary` (research use with attribution) | `ai/scripts/live_vqc_to_corpus_jsonl.py`, `docs/ai/live-vqc-ingestion.md` | Sinno and Bovik, IEEE TIP 2019. |
| [LSVQ (Patch-VQ)](https://github.com/baidut/PatchVQ) | integrated | `CC-BY-4.0` (as the Hugging Face mirror states it) | `ai/scripts/lsvq_to_corpus_jsonl.py`, `docs/ai/lsvq-ingestion.md` | Ying, Mandal, Ghadiyaram and Bovik, CVPR 2021. |
| [Netflix Public Dataset](https://github.com/Netflix/vmaf/blob/0fb4152418d0351901e9c5fd2d30668dced89cdb/resource/doc/datasets.md) | integrated | `none` (the page states no terms beyond requesting access) | `docs/models/datasets.md` | Access on request. Trained on it: fr_regressor v1 to v3 and the vmaf_tiny models. |
| [Waterloo IVC 4K-VQA](https://ivc.uwaterloo.ca/database/4KVQA.html) | integrated | `proprietary` (permissive academic licence, attribution required) | `ai/scripts/waterloo_ivc_to_corpus_jsonl.py`, `docs/ai/waterloo-ivc-4k-ingestion.md` | Li, Duanmu, Liu and Wang, ICIAR 2019. |
| [YouTube UGC Dataset](https://research.google/pubs/youtube-ugc-dataset-for-video-compression-research/) | integrated | `CC-BY-4.0` (the ATTRIBUTION file of the dataset states CC BY 4.0 for the listed clips) | `ai/scripts/youtube_ugc_to_corpus_jsonl.py`, `docs/ai/youtube-ugc-ingestion.md` | Wang, Inguva and Adsumilli, MMSP 2019. |
<!-- credits:end -->

## Papers and standards

Methods that VMAFx implements from the published description. Where an author's
code exists, it is listed above under its own licence. The note of each row names
the repository page that cites it.

<!-- credits:table papers -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [Bampis, Gupta, Soundararajan and Bovik, SpEED-QA (IEEE SPL 2017)](https://doi.org/10.1109/lsp.2017.2726542) | adapted | `unknown` | see the note | SpEED extractors. |
| [CAMBI banding detector (Tandon et al., PCS 2021)](https://netflixtechblog.medium.com/cambi-a-banding-artifact-detector-96777ae12fe2) | adapted | `BSD-2-Clause-Patent` | see the note | The paper is mirrored in docs/reference/papers; REUSE.toml records BSD-2-Clause-Patent for the PDF. |
| [Ding, Ma, Wang and Simoncelli, Image Quality Assessment: Unifying Structure and Texture Similarity (arXiv:2004.07728)](https://arxiv.org/abs/2004.07728) | inspired | `proprietary` (arXiv non-exclusive distribution licence) | see the note | DISTS. |
| [ITU-R BT.2022, general viewing conditions for subjective assessment](https://www.itu.int/rec/R-REC-BT.2022) | adapted | `unknown` | see the note | Viewing distance of the VMAF training experiments. |
| [ITU-R BT.2100, image parameter values for HDR television](https://www.itu.int/rec/R-REC-BT.2100) | adapted | `unknown` | see the note | RGB to LMS matrix, PQ transfer function, ICtCp. |
| [ITU-R BT.2124-0 (2019), potential visibility of colour differences in television](https://www.itu.int/rec/R-REC-BT.2124) | adapted | `unknown` | see the note | Annex 1 pipeline and Annex 4 worked example behind delta_e_itp. |
| [Li, Zhang, Ma and Ngan, Image quality assessment by separately evaluating detail losses and additive impairments (IEEE TMM 2011)](https://doi.org/10.1109/tmm.2011.2152382) | adapted | `unknown` | see the note | The detail-loss measure behind ADM. |
| [Mantiuk and Azimi, PU21 (QoMEX and PCS 2021)](https://ieeexplore.ieee.org/document/9477471/) | adapted | `unknown` | see the note | PU21 HDR adapter; the official code is credited separately. |
| [MISRA C:2012](https://misra.org.uk/) | inspired | `proprietary` | `docs/principles.md` | Informative subset only. |
| [Mittal, Moorthy and Bovik, No-reference image quality assessment in the spatial domain (IEEE TIP 2012)](https://doi.org/10.1109/tip.2012.2214050) | adapted | `unknown` | see the note | BRISQUE extractor. |
| [Mittal, Soundararajan and Bovik, Making a completely blind image quality analyzer (IEEE SPL 2013)](https://doi.org/10.1109/lsp.2012.2227726) | adapted | `unknown` | see the note | NIQE extractor. |
| [NASA/JPL Power of 10 and the JPL C coding standard](https://spinroot.com/gerard/pdf/P10.pdf) | adapted | `unknown` | `docs/principles.md` | Coding rules enforced through .clang-tidy and the praetor HISS invariants. |
| [SEI CERT C Coding Standard](https://wiki.sei.cmu.edu/confluence/display/c) | adapted | `unknown` | `docs/principles.md` | Mandatory rules of the C coding standard. |
| [Sheikh and Bovik, Image information and visual quality (IEEE TIP 2006)](https://doi.org/10.1109/tip.2005.859378) | adapted | `unknown` | see the note | VIF; luma only by design. |
| [SMPTE ST 2084:2014, Perceptual Quantizer EOTF](https://doi.org/10.5594/SMPTE.ST2084.2014) | adapted | `unknown` | see the note | PQ code values to absolute luminance. |
| [Soucek and Lokoc, TransNet V2 (arXiv:2008.04838)](https://arxiv.org/abs/2008.04838) | adapted | `proprietary` (arXiv non-exclusive distribution licence) | see the note | Shot-boundary detector. |
| [Tassano, Delon and Veit, FastDVDnet (arXiv:1907.01361)](https://arxiv.org/abs/1907.01361) | inspired | `proprietary` (arXiv non-exclusive distribution licence) | see the note | Temporal pre-filter. |
| [Toward a better quality metric for the video community (Netflix, 2020)](https://netflixtechblog.com/toward-a-better-quality-metric-for-the-video-community-7ed94e752a30) | inspired | `unknown` | see the note | Speed work, the API and the NEG mode. |
| [Toward a practical perceptual video quality metric (Netflix, 2016)](https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652) | inspired | `unknown` | see the note | The tech blog that open sourced VMAF. |
| [Venkataramanan, Stejerean, Katsavounidis and Bovik, One Transform To Compute Them All (arXiv:2304.03412)](https://arxiv.org/abs/2304.03412) | adapted | `CC-BY-4.0` (the arXiv listing names it) | see the note | Y-FUNQUE+ atom features. |
| [Wu, Liu, Cheng, Lu and Cheng, MobileSal (arXiv:2012.13095)](https://arxiv.org/abs/2012.13095) | inspired | `CC-BY-NC-SA-4.0` (the arXiv listing names it) | see the note | Saliency extractor design. |
| [Zhang, Isola, Efros, Shechtman and Wang, The Unreasonable Effectiveness of Deep Features as a Perceptual Metric (arXiv:1801.03924)](https://arxiv.org/abs/1801.03924) | inspired | `proprietary` (arXiv non-exclusive distribution licence) | see the note | LPIPS. |
<!-- credits:end -->

## Adapted texts and inspirations

<!-- credits:table texts -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [Caveman](https://github.com/JuliusBrussee/caveman) | adapted | `MIT` (terms at adaptation; upstream relicensed to Apache-2.0 on 2026-09-24) | see the note | Julius Brussee and contributors; the internal text register skill is adapted from it. |
| [Contributor Covenant 2.1](https://www.contributor-covenant.org/version/2/1/code_of_conduct/) | adapted | `unknown` (the page states no licence for the text) | `CODE_OF_CONDUCT.md` | Our code of conduct, with the enforcement guidelines of the same version. |
| [Conventional Commits](https://www.conventionalcommits.org/) | inspired | `CC-BY-3.0` (as the specification page states it) | `CONTRIBUTING.md` | Commit subjects and release automation. |
| [Developer Certificate of Origin 1.1](https://developercertificate.org/) | integrated | `unknown` (the text allows verbatim copies only) | `CONTRIBUTING.md` | The sign-off certifies it; the text is referenced, not copied. |
| [Documenting Architecture Decisions (Michael Nygard)](https://www.cognitect.com/blog/2011/11/15/documenting-architecture-decisions) | inspired | `unknown` | `docs/adr/0000-template.md` | The ADR template follows this format. |
| [i-have-adhd](https://github.com/ayghri/i-have-adhd) | adapted | `MIT` | see the note | Source of the adhd-format and social-text skills. |
| [Keep a Changelog](https://github.com/olivierlacan/keep-a-changelog) | inspired | `MIT` | `changelog.d/README.md` | Section names of the changelog fragments. |
| [Mozilla community enforcement ladder](https://github.com/mozilla/diversity) | inspired | `MPL-2.0` | `CODE_OF_CONDUCT.md` | The Community Impact Guidelines of the Covenant were inspired by it. |
<!-- credits:end -->

## Build and CI tools, actions and images

<!-- credits:table tools -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [actionlint](https://github.com/rhysd/actionlint) | used-by-CI | `MIT` | `.github/AGENTS.d/required-aggregator.md`, `.github/actionlint.yaml`, `.github/workflows/ffmpeg-integration.yml`, `.pre-commit-config.yaml`, and 2 more |  |
| [actions/attest](https://github.com/actions/attest) | used-by-CI | `MIT` | `.github/actions/image-licence-artifacts/action.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, `.github/workflows/docker-publish-tester.yml`, and 3 more |  |
| [actions/attest-build-provenance](https://github.com/actions/attest-build-provenance) | used-by-CI | `MIT` | `.github/actions/image-licence-artifacts/action.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, `.github/workflows/docker-publish-tester.yml`, and 3 more |  |
| [actions/cache](https://github.com/actions/cache) | used-by-CI | `MIT` | `.github/workflows/build.yml`, `.github/workflows/e2e-k8s.yml`, `.github/workflows/libvmaf-build-matrix.yml`, `.github/workflows/lint-and-format.yml`, and 3 more |  |
| [actions/checkout](https://github.com/actions/checkout) | used-by-CI | `MIT` | `.github/workflows/build.yml`, `.github/workflows/ci-escalate.yml`, `.github/workflows/ci-tier.yml`, `.github/workflows/dev-container-build.yml`, and 4 more |  |
| [actions/create-github-app-token](https://github.com/actions/create-github-app-token) | used-by-CI | `MIT` | `.github/workflows/macos-tester-bundle.yml`, `.github/workflows/release-please.yml`, `.github/workflows/windows-tester-bundle.yml` |  |
| [actions/dependency-review-action](https://github.com/actions/dependency-review-action) | used-by-CI | `MIT` | `.github/workflows/security-scans.yml` |  |
| [actions/deploy-pages](https://github.com/actions/deploy-pages) | used-by-CI | `MIT` | `.github/workflows/docs.yml` |  |
| [actions/download-artifact](https://github.com/actions/download-artifact) | used-by-CI | `MIT` | `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-tester.yml`, `.github/workflows/e2e-k8s.yml`, `.github/workflows/macos-tester-bundle.yml`, and 4 more |  |
| [actions/github-script](https://github.com/actions/github-script) | used-by-CI | `MIT` | `.github/workflows/required-aggregator.yml` |  |
| [actions/setup-go](https://github.com/actions/setup-go) | used-by-CI | `MIT` | `.github/workflows/go-ci.yml`, `.github/workflows/praetor-api.yml`, `.github/workflows/published-rc-licence-companions.yml`, `.github/workflows/standards-gate.yml` |  |
| [actions/setup-node](https://github.com/actions/setup-node) | used-by-CI | `MIT` | `.github/workflows/lint-and-format.yml`, `.github/workflows/praetor-docs.yml` |  |
| [actions/setup-python](https://github.com/actions/setup-python) | used-by-CI | `MIT` | `.github/workflows/build.yml`, `.github/workflows/docs.yml`, `.github/workflows/ffmpeg-integration.yml`, `.github/workflows/libvmaf-build-matrix.yml`, and 4 more |  |
| [actions/upload-artifact](https://github.com/actions/upload-artifact) | used-by-CI | `MIT` | `.github/actions/image-licence-artifacts/action.yml`, `.github/workflows/build.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, and 4 more |  |
| [actions/upload-pages-artifact](https://github.com/actions/upload-pages-artifact) | used-by-CI | `MIT` | `.github/workflows/docs.yml` |  |
| [anchore/sbom-action](https://github.com/anchore/sbom-action) | used-by-CI | `Apache-2.0` | `.github/actions/image-licence-artifacts/action.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, `.github/workflows/docker-publish-tester.yml`, and 4 more |  |
| [Black](https://github.com/psf/black) | used-by-CI | `MIT` | `.github/copilot-instructions.md`, `.github/workflows/lint-and-format.yml`, `.pre-commit-config.yaml`, `Makefile`, and 4 more |  |
| [cargo-deny](https://github.com/EmbarkStudios/cargo-deny) | used-by-CI | `Apache-2.0` | `.github/workflows/required-aggregator.yml`, `.github/workflows/rust-ci.yml`, `mkdocs.yml`, `scripts/ci/tests/test_ci_impact.py`, and 2 more |  |
| [Cppcheck](https://github.com/danmar/cppcheck) | used-by-CI | `GPL-3.0-or-later` | `.github/AGENTS.d/cppcheck-posix-model.md`, `.github/AGENTS.md`, `.github/agents/c-reviewer.md`, `.github/ci-impact.json`, and 4 more |  |
| [Cython](https://github.com/cython/cython) | used-by-CI | `Apache-2.0` | `.github/test-suites.json`, `.github/workflows/lint-and-format.yml`, `docker/Dockerfile.tester`, `noxfile.py`, and 4 more | Builds the Python harness extension. |
| [Debian base image](https://hub.docker.com/_/debian) | shipped | `unknown` | `build-config.env`, `docker/AGENTS.md`, `docker/Dockerfile.controller`, `docker/Dockerfile.node`, and 4 more | Image of many packages under their own licences; the per-image notices list them (docs/licensing.md). |
| [Distroless base images](https://github.com/GoogleContainerTools/distroless) | shipped | `Apache-2.0` | `build-config.env`, `docker/AGENTS.md`, `docker/Dockerfile.controller`, `docker/Dockerfile.node`, and 2 more |  |
| [docker/build-push-action](https://github.com/docker/build-push-action) | used-by-CI | `Apache-2.0` | `.github/actions/image-licence-artifacts/action.yml`, `.github/workflows/dev-container-publish.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, and 3 more |  |
| [docker/login-action](https://github.com/docker/login-action) | used-by-CI | `Apache-2.0` | `.github/workflows/dev-container-publish.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, `.github/workflows/docker-publish-tester.yml`, and 1 more |  |
| [docker/metadata-action](https://github.com/docker/metadata-action) | used-by-CI | `Apache-2.0` | `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml` |  |
| [docker/setup-buildx-action](https://github.com/docker/setup-buildx-action) | used-by-CI | `Apache-2.0` | `.github/workflows/dev-container-publish.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, `.github/workflows/docker-publish-tester.yml`, and 3 more |  |
| [docker/setup-qemu-action](https://github.com/docker/setup-qemu-action) | used-by-CI | `Apache-2.0` | `.github/workflows/docker-publish-production.yml` |  |
| [Doxygen](https://github.com/doxygen/doxygen) | used-by-CI | `unknown` | `.github/agents/hip-reviewer.md`, `.github/agents/metal-reviewer.md`, `.github/ci-impact.json`, `.github/codeql-config.yml`, and 4 more |  |
| [dtolnay/rust-toolchain](https://github.com/dtolnay/rust-toolchain) | used-by-CI | `MIT` | `.github/workflows/rust-ci.yml` |  |
| [EmbarkStudios/cargo-deny-action](https://github.com/EmbarkStudios/cargo-deny-action) | used-by-CI | `Apache-2.0` | `.github/workflows/rust-ci.yml` |  |
| [EnricoMi/publish-unit-test-result-action](https://github.com/EnricoMi/publish-unit-test-result-action) | used-by-CI | `Apache-2.0` | `.github/workflows/e2e-k8s.yml` |  |
| [gcovr](https://github.com/gcovr/gcovr) | used-by-CI | `BSD-3-Clause` | `.github/workflows/tests-and-quality-gates.yml`, `Makefile`, `requirements/locks/gcovr.in`, `requirements/locks/gcovr.txt`, and 4 more |  |
| [github/codeql-action](https://github.com/github/codeql-action) | used-by-CI | `MIT` | `.github/workflows/scorecard.yml`, `.github/workflows/security-scans.yml` |  |
| [Gitleaks](https://github.com/gitleaks/gitleaks) | used-by-CI | `MIT` | `.github/ci-impact.json`, `.github/workflows/required-aggregator.yml`, `.github/workflows/security-scans.yml`, `.pre-commit-config.yaml`, and 2 more |  |
| [Go base image](https://hub.docker.com/_/golang) | used-by-CI | `unknown` | `build-config.env`, `docker/Dockerfile.controller`, `docker/Dockerfile.node`, `docker/Dockerfile.operator` | Same note as the Debian image. |
| [googleapis/release-please-action](https://github.com/googleapis/release-please-action) | used-by-CI | `Apache-2.0` | `.github/workflows/release-please.yml` |  |
| [Hadolint](https://github.com/hadolint/hadolint) | used-by-CI | `unknown` | `Dockerfile`, `Dockerfile.ffmpeg`, `Dockerfile.go-server`, `dev/AGENTS.d/shell-contract.md`, and 4 more |  |
| [Helm](https://github.com/helm/helm) | used-by-CI | `Apache-2.0` | `.github/AGENTS.d/helm-and-e2e.md`, `.github/AGENTS.md`, `.github/ci-impact.json`, `.github/copilot-instructions.md`, and 4 more | Chart packaging and tests. |
| [Intel compute-runtime (NEO)](https://github.com/intel/compute-runtime) | integrated | `MIT` | `build-config.env`, `dev/AGENTS.d/runtime-dependencies.md`, `dev/AGENTS.d/uapi-version-pins.md`, `dev/AGENTS.md`, and 4 more | Intel GPU compute stack in the dev image. |
| [Intel oneAPI Base Toolkit](https://www.intel.com/content/www/us/en/developer/tools/oneapi/base-toolkit.html) | integrated | `unknown` | `Dockerfile`, `build-config.env`, `dev/AGENTS.d/gpu-backend-exposure.md`, `dev/AGENTS.d/package-names.md`, and 4 more | Toolkit packages installed in the SYCL images. |
| [Intel oneAPI Base Toolkit image](https://hub.docker.com/r/intel/oneapi-basekit) | used-by-CI | `unknown` | `.github/workflows/docker-publish-production.yml`, `docker/AGENTS.md`, `docker/dev/ubuntu-26.04-sycl.Dockerfile` |  |
| [Intel oneAPI DPC++ (intel/llvm)](https://github.com/intel/llvm) | integrated | `Apache-2.0 WITH LLVM-exception` | `build-config.env`, `dev/AGENTS.d/uapi-version-pins.md`, `dev/Containerfile`, `dev/scripts/fetch-intel-neo.py`, and 2 more | SYCL compiler. |
| [kind](https://github.com/kubernetes-sigs/kind) | used-by-CI | `Apache-2.0` | `.github/AGENTS.d/build-matrix-record.md`, `.github/AGENTS.d/helm-and-e2e.md`, `.github/copilot-instructions.md`, `.github/workflows/e2e-k8s.yml`, and 4 more | Cluster for the end-to-end tests. |
| [Lefthook](https://github.com/evilmartians/lefthook) | used-by-CI | `MIT` | `.github/ci-impact.json`, `.github/copilot-instructions.md`, `.github/workflows/standards-gate.yml`, `.pre-commit-config.yaml`, and 4 more |  |
| [LLVM (clang, clang-tidy, clang-format)](https://github.com/llvm/llvm-project) | used-by-CI | `Apache-2.0 WITH LLVM-exception` | `.github/AGENTS.d/hosted-runner-packages.md`, `.github/AGENTS.md`, `.github/agents/c-reviewer.md`, `.github/ci-impact.json`, and 4 more | Lint and format gates, and the CUDA clang driver. |
| [markdownlint-cli2](https://github.com/DavidAnson/markdownlint-cli2) | used-by-CI | `MIT` | `.github/AGENTS.d/dev-image-private-guard.md`, `.github/PULL_REQUEST_TEMPLATE.md`, `.github/agents/c-reviewer.md`, `.github/agents/cuda-reviewer.md`, and 4 more |  |
| [Material for MkDocs](https://github.com/squidfunk/mkdocs-material) | used-by-CI | `MIT` | `mkdocs.yml` |  |
| [Meson](https://github.com/mesonbuild/meson) | used-by-CI | `Apache-2.0` | `.github/AGENTS.d/build-matrix-record.md`, `.github/AGENTS.d/hosted-runner-packages.md`, `.github/AGENTS.d/meson-sanitization.md`, `.github/AGENTS.d/release-fan-out.md`, and 4 more | Build system of libvmaf. |
| [MkDocs](https://github.com/mkdocs/mkdocs) | used-by-CI | `BSD-2-Clause` | `.github/PULL_REQUEST_TEMPLATE.md`, `.github/agents/doc-reviewer.md`, `.github/ci-impact.json`, `.github/workflows/docs.yml`, and 4 more | Documentation site generator. |
| [msys2/setup-msys2](https://github.com/msys2/setup-msys2) | used-by-CI | `MIT` | `.github/workflows/libvmaf-build-matrix.yml` |  |
| [mxschmitt/action-tmate](https://github.com/mxschmitt/action-tmate) | used-by-CI | `MIT` | `.github/AGENTS.d/macos-tmate.md`, `.github/workflows/libvmaf-build-matrix.yml` |  |
| [mypy](https://github.com/python/mypy) | used-by-CI | `MIT` | `.github/workflows/lint-and-format.yml`, `.github/workflows/tests-and-quality-gates.yml`, `.pre-commit-config.yaml`, `Makefile`, and 4 more |  |
| [NASM](https://github.com/netwide-assembler/nasm) | used-by-CI | `BSD-2-Clause` | `.github/AGENTS.d/windows-runners.md`, `.github/workflows/build.yml`, `.github/workflows/ffmpeg-integration.yml`, `.github/workflows/fuzz.yml`, and 4 more | Assembler of the x86 SIMD code. |
| [Ninja](https://github.com/ninja-build/ninja) | used-by-CI | `Apache-2.0` | `.github/AGENTS.d/meson-sanitization.md`, `.github/AGENTS.d/security-scans.md`, `.github/AGENTS.d/windows-runners.md`, `.github/AGENTS.md`, and 4 more |  |
| [nox](https://github.com/wntrblm/nox) | used-by-CI | `Apache-2.0` | `.github/workflows/tests-and-quality-gates.yml`, `noxfile.py`, `requirements/AGENTS.md`, `requirements/locks/manifest.json`, and 4 more |  |
| [NVIDIA CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit) | integrated | `proprietary` | `Dockerfile`, `build-config.env`, `dev/AGENTS.d/ffmpeg-encoders.md`, `dev/AGENTS.d/package-names.md`, and 4 more | Compiler and headers; the CUDA kernels contain NVIDIA code from the toolkit headers and libdevice (docs/licensing.md). |
| [oneAPI Level Zero](https://github.com/oneapi-src/level-zero) | integrated | `MIT` | `Dockerfile`, `dev/AGENTS.d/gpu-backend-exposure.md`, `dev/AGENTS.d/uapi-version-pins.md`, `dev/Containerfile`, and 4 more | Device runtime of the SYCL backend. |
| [ONNX Runtime (C library)](https://github.com/microsoft/onnxruntime) | integrated | `MIT` | `docker/Dockerfile.production`, `docs/ai/inference.md` | Inference engine of the tiny-AI surface. |
| [ossf/scorecard-action](https://github.com/ossf/scorecard-action) | used-by-CI | `Apache-2.0` | `.github/workflows/scorecard-policy.yml`, `.github/workflows/scorecard.yml` |  |
| [OSV-Scanner](https://github.com/google/osv-scanner) | used-by-CI | `Apache-2.0` | `.github/ci-impact.json`, `scripts/ci/tests/test_ci_impact.py` |  |
| [praetor](https://github.com/cordanaLLM/praetor) | used-by-CI | `EUPL-1.2` | `.standards.yaml`, `.github/workflows/standards-gate.yml` | Standards engine, hooks and skills; installed by praetorctl adopt. |
| [pre-commit](https://github.com/pre-commit/pre-commit) | used-by-CI | `MIT` | `.github/AGENTS.d/pelorus-mirror.md`, `.github/AGENTS.md`, `.github/ci-impact.json`, `.github/copilot-instructions.md`, and 4 more |  |
| [pypa/gh-action-pypi-publish](https://github.com/pypa/gh-action-pypi-publish) | used-by-CI | `BSD-3-Clause` | `.github/workflows/supply-chain.yml` |  |
| [pytest](https://github.com/pytest-dev/pytest) | used-by-CI | `MIT` | `.github/test-suites.json`, `.github/workflows/build.yml`, `.github/workflows/mini-retrain.yml`, `.github/workflows/tests-and-quality-gates.yml`, and 4 more |  |
| [Python base image](https://hub.docker.com/_/python) | shipped | `unknown` | `Dockerfile`, `build-config.env`, `docker/Dockerfile.controller`, `docker/Dockerfile.node`, and 4 more | Same note as the Debian image. |
| [QEMU](https://github.com/qemu/qemu) | used-by-CI | `unknown` | `.github/copilot-instructions.md`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, `Dockerfile.go-server`, and 4 more | User-mode emulation for the Arm and SVE2 runs. |
| [rclone](https://github.com/rclone/rclone) | integrated | `MIT` | `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/go-ci.yml`, `build-config.env`, `deploy/helm/vmafx/AGENTS.md`, and 4 more | Mover in the node image; the eBPF program bypasses its reads. |
| [release-please](https://github.com/googleapis/release-please) | used-by-CI | `Apache-2.0` | `.github/AGENTS.d/release-fan-out.md`, `.github/AGENTS.d/required-aggregator.md`, `.github/AGENTS.md`, `.github/CODEOWNERS`, and 4 more | Release pull requests and tags. |
| [Renovate](https://github.com/renovatebot/renovate) | used-by-CI | `AGPL-3.0-only` | `.github/AGENTS.d/build-matrix-record.md`, `.github/AGENTS.d/container-signing.md`, `.github/AGENTS.d/macos-tmate.md`, `.github/AGENTS.d/python-requirement.md`, and 4 more | Dependency updates. |
| [REUSE tool](https://github.com/fsfe/reuse-tool) | used-by-CI | `Apache-2.0 AND CC0-1.0 AND CC-BY-SA-4.0 AND GPL-3.0-or-later` | `.github/ci-impact.json`, `.github/copilot-instructions.md`, `.github/test-suites.json`, `.github/workflows/docker-publish-tester.yml`, and 4 more | Licence metadata checks. |
| [ROCm](https://github.com/ROCm/ROCm) | integrated | `MIT` | `dev/Containerfile`, `dev/docker-compose.yml` | HIP compiler stack. |
| [ROCm development image](https://hub.docker.com/r/rocm/dev-ubuntu-26.04) | shipped | `unknown` | `build-config.env`, `docker/Dockerfile.node`, `docker/Dockerfile.production-gpu`, `docker/Dockerfile.tester` |  |
| [Ruff](https://github.com/astral-sh/ruff) | used-by-CI | `MIT` | `.github/copilot-instructions.md`, `.github/workflows/lint-and-format.yml`, `.github/workflows/tests-and-quality-gates.yml`, `.pre-commit-config.yaml`, and 4 more |  |
| [Semgrep](https://github.com/semgrep/semgrep) | used-by-CI | `LGPL-2.1-or-later` | `.github/AGENTS.d/security-scans.md`, `.github/AGENTS.md`, `.github/ci-impact.json`, `.github/copilot-instructions.md`, and 4 more |  |
| [ShellCheck](https://github.com/koalaman/shellcheck) | used-by-CI | `GPL-3.0-only` | `.github/workflows/ffmpeg-integration.yml`, `.github/workflows/go-ci.yml`, `.github/workflows/libvmaf-build-matrix.yml`, `.github/workflows/lint-and-format.yml`, and 4 more |  |
| [shfmt (mvdan/sh)](https://github.com/mvdan/sh) | used-by-CI | `BSD-3-Clause` | `.github/workflows/lint-and-format.yml`, `.github/workflows/required-aggregator.yml`, `.pre-commit-config.yaml`, `Makefile`, and 3 more |  |
| [Sigstore cosign](https://github.com/sigstore/cosign) | used-by-CI | `Apache-2.0` | `.github/AGENTS.d/container-signing.md`, `.github/AGENTS.md`, `.github/actions/image-licence-artifacts/action.yml`, `.github/workflows/dev-container-publish.yml`, and 4 more | Keyless signing of release artifacts. |
| [sigstore/cosign-installer](https://github.com/sigstore/cosign-installer) | used-by-CI | `Apache-2.0` | `.github/workflows/dev-container-publish.yml`, `.github/workflows/docker-publish-operator-node.yml`, `.github/workflows/docker-publish-production.yml`, `.github/workflows/docker-publish-tester.yml`, and 4 more |  |
| [softprops/action-gh-release](https://github.com/softprops/action-gh-release) | used-by-CI | `MIT` | `.github/workflows/supply-chain.yml` |  |
| [Swatinem/rust-cache](https://github.com/Swatinem/rust-cache) | used-by-CI | `unknown` | `.github/workflows/rust-ci.yml` |  |
| [Syft](https://github.com/anchore/syft) | used-by-CI | `Apache-2.0` | `.github/AGENTS.d/container-signing.md`, `.github/actions/image-licence-artifacts/action.yml`, `.github/actions/image-licence-artifacts/sbom.sh`, `.github/workflows/docker-publish-operator-node.yml`, and 4 more | SBOM generation. |
| [TheMrMilchmann/setup-msvc-dev](https://github.com/TheMrMilchmann/setup-msvc-dev) | used-by-CI | `MIT` | `.github/workflows/build.yml`, `.github/workflows/libvmaf-build-matrix.yml`, `.github/workflows/windows-tester-bundle.yml` |  |
| [tox](https://github.com/tox-dev/tox) | used-by-CI | `MIT` | `.github/test-suites.json`, `.github/workflows/build.yml`, `.github/workflows/libvmaf-build-matrix.yml`, `.github/workflows/tests-and-quality-gates.yml`, and 4 more |  |
| [Ubuntu base image](https://hub.docker.com/_/ubuntu) | shipped | `unknown` | `Dockerfile`, `build-config.env`, `dev/AGENTS.d/entrypoint-unprivileged.md`, `dev/AGENTS.d/ffmpeg-encoders.md`, and 4 more | Same note as the Debian image. |
| [vl-convert](https://github.com/vega/vl-convert) | used-by-CI | `BSD-3-Clause` | `scripts/docs/generate-charts.py` | Renders the documentation charts and writes the Vega bundle. |
<!-- credits:end -->

## Fonts

<!-- credits:table fonts -->
| Item | Relation | Licence | Where it is used | Note |
| --- | --- | --- | --- | --- |
| [Inter](https://github.com/rsms/inter) | vendored | `OFL-1.1` | `docs/assets/fonts/inter/` | Subset served by the documentation site (ADR-1508). |
| [JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) | vendored | `OFL-1.1` | `docs/assets/fonts/jetbrains-mono/` | Subset served by the documentation site (ADR-1508). |
<!-- credits:end -->

## Add an entry

You added, copied or learned from something that is not VMAFx's own work. To
credit it:

1. Open [`docs/credits.yaml`](https://github.com/VMAFx/vmafx/blob/master/docs/credits.yaml)
   and add one entry. The fields are `id` (lower-case kebab), `name`, `url` (the
   upstream, `https`), `kind` (`upstream`, `code`, `library`, `tool`, `action`,
   `image`, `font`, `model`, `dataset`, `paper`, `standard` or `text`),
   `relation` (the table above), `license`, and optionally `license_note`,
   `paths` (repository paths or globs that use it), `evidence` (repository files
   that state the facts you wrote, required in spirit for models, datasets and
   papers) and `note`.
2. Copy the licence from the upstream. If it names none, write `none`; if you
   cannot tell, write `unknown`. Never guess.
3. Run `make docs-fragments-write` to render the tables, then
   `make docs-fragments-check`.

`make docs-fragments-check` runs `scripts/docs/generate-credits.py --check` and
`scripts/docs/check-credits.py`. The second fails when:

- the tables differ from the list (page drift);
- a vendored or inherited third-party path has no entry: any non-project
  licence in `REUSE.toml`, any `third_party`, `3rdparty` or `vendor` directory,
  notice and licence files, font files, and source files whose copyright header
  names someone other than the project and its contributors;
- a `LICENSES/*.txt` text is used by no entry and no project code;
- a skill or agent file says it was derived from an upstream (`derived_from`, or
  "Adapted from [name](url)") and no entry has that URL, which is how files that
  `praetorctl adopt` installs are held to their credit;
- an entry names a path or an evidence file that is not in the checkout.

Each check has a planted-defect test in
`scripts/docs/tests/test_credits.py` that the gate must fail on. The decision is
[ADR-2485](adr/2485-vmafx-credits-page.md).
<!-- REUSE-IgnoreEnd -->
