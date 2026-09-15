- **BREAKING: fork-authored code is now EUPL-1.2; the shipped library is
  effectively copyleft** (ADR-1250). Code inherited from or derived from
  Netflix/vmaf keeps BSD-2-Clause-Patent unchanged; 1,622 fork-authored files move
  to [EUPL-1.2](LICENSES/EUPL-1.2.txt). Because `libvmaf` links both together,
  **redistributing a modified library now obliges you to offer its source under
  EUPL-1.2**. If you need permissive terms, use
  [Netflix/vmaf](https://github.com/Netflix/vmaf) upstream, which is unaffected.
  The MIT alternative is withdrawn from 123 Go files under `pkg/`,
  `cmd/vmafx-tune` and `cmd/vmafx-node/bpf`; anyone who already received those
  files under MIT keeps that grant for those versions.
  The change also repairs licence metadata that was simply wrong: 946 files
  declared `SPDX-License-Identifier: BSD-3-Clause-Plus-Patent`, which is not a
  real SPDX identifier, one declared the deprecated `BSD+Patent`, 223 stated
  their terms only in prose with no machine-readable tag, and 147 carried no
  notice at all. Which files moved was decided mechanically, by provenance rather
  than by reading headers: no counterpart path upstream, no matching filename
  upstream, and no copyright line other than Lusoris. 630 files were deliberately
  left alone under that rule, including Netflix's relocated Python tree and the
  arm64 NEON kernels that carry Netflix's copyright. 95 files in that excluded set
  still declare the invalid identifier and are tracked as follow-up work, because
  correcting them means determining the real terms of a third-party derivative.
