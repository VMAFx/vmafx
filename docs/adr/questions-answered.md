<!-- markdownlint-disable MD013 -->
# Questions Answered (verbatim)

Q&A from the in-agent popup rounds during planning. User replies are verbatim.

---

## Round 1 — Critical merge / dir / push mechanics

**Q1.1** — There's an uncommitted change to `testdata/netflix_benchmark_results.json`. What should I do with it before merging?
> **User:** _"lol those are just results? thats not really a commit? O.o"_
> **Interpretation:** benchmark noise, not a real change → stash, drop later. → **D1**

**Q1.2** — How should `sycl-gpu-optimizations` merge into `sycl`, and `sycl` into 'main'?
> **User:** _"alter ich will dass mein main einfach der head wird und wir haben ja upstream, brauch doch keine eigene branch wenn ich danach changes in branches erstelle und wieder merge, no? like golusoris describes?"_
> **Interpretation:** collapse branches, make master the fork's real dev branch (golusoris-style), upstream/master still available via `git fetch upstream`. → **D2**

**Q1.3** — What is `.workingdir2`?
> **User selected:** _"New empty dir alongside existing .workingdir"_ → **D3**

**Q1.4** — Push to origin after merging?
> **User:** _"alter, mach das, das sind ja die dämlichsten fragen und ich schätze/hoffe da kommen noch viel viel mehr mit mehr sinn etc..."_
> **Interpretation:** yes, push; user wants substantive questions next. → **D4**

---

## Round 2 — Framework scope, precision, settings, branding

**Q2.1** — Which parts of the golusoris framework should I port?
> **User selected:** _"Everything (a–g)"_ → **D5**

**Q2.2** — How should "show the full output" manifest for CLI precision?
> **User:** _"well the best and professional solution i guess"_
> **Interpretation:** non-committal; needs a follow-up. → clarified in Round 3 → **D6**

**Q2.3** — Merge existing `.claude/settings.json` or fresh?
> **User selected:** _"Fresh — rewrite from scratch"_ → **D7**

**Q2.4** — Branding direction for README?
> **User:** _"well number one but of course we have to keep licensing in tact etc... so just additions I assume"_
> **Interpretation:** distinct lusoris fork README with additions; preserve BSD+Patent license + Netflix attribution. → **D8**

---

## Round 3 — Precision, kofi, MCP, signing

**Q3.1** — Precise precision model?
> **User:** _"well the fucking py testsuit for cpu only produces an amount of digits and puts it out, thats what the cli has to do as well... or the comparison always sucks because of rounding"_
> **Interpretation:** the CLI must emit enough digits that comparisons against Python results don't round. → presented in Round 3 final question → **D6** (`%.17g` default + `--precision=N`)

**Q3.2** — Kofi handle?
> **User:** _"alter der ist doch in golusoris drin und es ist kofi lusoris, das denke ich mir doch nicht einfach aus wtf..."_
> **Interpretation:** `lusoris` (kofi.com/lusoris). → **D8**

**Q3.3** — MCP scope?
> **User selected (multi):** _"Run vmaf score (ref+dist video inputs, model selection), List available models, List GPU backends (SYCL/CUDA devices available), Run benchmarks (vmaf_bench harness)"_ → **D9**

**Q3.4** — Signing path?
> **User selected:** _"Keyless via GitHub OIDC + Sigstore (Recommended)"_ → **D10**

---

## Round 3.5 — Precision final + branching + versioning

**Q3.5.1** — `%.17g` everywhere default + `--precision=N`?
> **User selected:** _"Yes, %.17g default everywhere + --precision=N (Recommended)"_ → **D6**

**Q3.5.2** — Concrete branching model?
> **User selected:** _"gpu-opt→sycl→master, set master as default on origin (Recommended)"_ → **D2**

**Q3.5.3** — Versioning?
> **User selected:** _"lusoris-fork versions (e.g., v3.0.0-lusoris.1)"_ → **D11**

---

## Round 4 — Distros, VSCode LSP, CI matrix, merge policy

**Q4.1** — Distro bundle?
> **User selected:** _"Ubuntu + Arch + Fedora + Alpine + macOS + Windows"_ → **D13**

**Q4.2** — VSCode LSP?
> **User selected:** _"clangd (Recommended)"_ → **D14**

**Q4.3** — CI matrix + sanitizers?
> **User selected:** _"Linux+macOS+Windows; ASan/UBSan every PR; TSan nightly (Recommended)"_ → **D15**

**Q4.4** — Merge conflict policy?
> **User selected:** _"Confirm policy as described (Recommended)"_ → **D16**

---

## Round 5 — Tiny AI scope (capabilities, training stack, runtime, surfaces)

**Q5.1** — Which golusoris `ai/` capability are we porting? (multi-select)
> **User selected (all four):** _"Custom VMAF quality models, Learned no-reference metrics, Learned filters for encoders, LLM dev helpers"_ → **D20**

**Q5.2** — Training stack?
> **User selected:** _"PyTorch + Lightning (Recommended)"_ → **D21**

**Q5.3** — Deployment runtime?
> **User selected:** _"ONNX Runtime (Recommended)"_ → **D22**

**Q5.4** — User-facing surfaces? (multi-select)
> **User selected (all four):** _"libvmaf C API: VmafModel from .onnx, vmaf CLI: --tiny-model PATH, ffmpeg filter: libvmaf=tiny_model=..., Standalone `vmaf-train` CLI"_ → **D23**

---

## Inline user directives (not popup-sourced)

- **"we should also add jpl coding guidelines"** → **D12** (NASA/JPL Power of 10 + JPL Institutional Coding Standard for C + SEI CERT + MISRA informative)
- **"claude skills and hooks need more, like adding a new gpu backend or whatever..."** → **D17** + **D18** (expanded skill catalog with domain scaffolding; comprehensive hooks catalog)
- **"lol is that one file in .workingdir2 really all your findings and analysis and plannings? thats a joke i guess"** → **D19** (restructured .workingdir2 into full dossier with README / PLAN / OPEN / analysis / decisions / phases)
- **"you should create setups then for a) the build and tests locally on different platforms and distros b) vscode project settings that have only but all the right tools for this project and nothing more"** → drove creation of Phase 3h (local dev setup) + Phase 3i (VSCode)
