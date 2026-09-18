- **BREAKING: fork-authored code is now EUPL-1.2, and the shipped library is
  effectively copyleft** (ADR-1250). Code the fork inherited, ported, copied or
  translated from someone else keeps the terms it already carried — Netflix's
  BSD-2-Clause-Patent, and the licences of the libjxl, Xiph and IQA code the fork
  builds on — and now carries that code's copyright notice, which those licences
  require and which had been missing. 1,514 fork-authored files move to
  [EUPL-1.2](LICENSES/EUPL-1.2.txt); 269 stay because they carry someone else's
  code. Because `libvmaf` links both together, **redistributing a modified library
  now obliges you to offer its source under EUPL-1.2**. If you need permissive
  terms, use [Netflix/vmaf](https://github.com/Netflix/vmaf) upstream, which is
  unaffected. The MIT alternative is withdrawn from 123 Go files under `pkg/`,
  `cmd/vmafx-tune` and `cmd/vmafx-node/bpf`; anyone who already received those
  files under MIT keeps that grant for those versions.
  The change also repairs licence metadata that was simply wrong: 946 files
  declared `SPDX-License-Identifier: BSD-3-Clause-Plus-Patent`, which is not a real
  SPDX identifier, 223 stated their terms only in prose with no machine-readable
  tag, and 147 carried no notice at all. Which files moved was decided by
  provenance rather than by reading headers, and is reproducible:
  `scripts/dev/relicense_fork_files.py --check` re-derives every verdict and fails
  if the tree disagrees. Four SYCL files carrying an outside contributor's work
  stay on their current terms pending that contributor's agreement.
