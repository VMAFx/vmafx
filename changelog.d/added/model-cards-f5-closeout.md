- **Model cards for 4 previously undocumented tiny-AI checkpoints**
  (`docs/ai/models/vmaf_tiny_v1.md`, `vmaf_tiny_v1_medium.md`,
  `smoke_v0.md`, `smoke_fp16_v0.md`): closes the ADR-0042 F5 gap
  identified in audit a9d16c8488283d193. Cards cover provenance,
  architecture, parameter counts, corpus references, evaluation
  posture, known limitations, and mkdocs nav entries.
- **Deleted 2 orphaned forward-looking stub cards**
  (`docs/ai/models/vmaf_tiny_v5.md`,
  `docs/ai/models/u2netp_mirror_card.md`): no binary exists in
  `model/tiny/` for either stub; content moved to their respective
  ADRs (ADR-0287, ADR-0412, ADR-0671) which remain authoritative.
