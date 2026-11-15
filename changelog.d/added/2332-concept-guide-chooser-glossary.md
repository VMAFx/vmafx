- **A concept guide, a metric chooser and a glossary explain what the score
  means and which metric answers which question, and a docs check holds the
  metric pages to the glossary (#2332).** `docs/reference/concepts.md` says
  what the 0–100 score predicts, the viewing assumptions baked into it
  (1080p at 3H), and the pitfalls that bend it — reversed FFmpeg pads
  (83.782079 vs 76.667830, from Research-0730), model and generation
  mismatch, HDR input with an SDR model, limited-vs-full range, frame
  alignment, the ~98.7 self-comparison, and metric-implementation
  differences — each with its recorded wrong/right numbers or a runnable
  command on the fixture pair. `docs/reference/chooser.md` is the decision
  table from question to metric, model and pitfall (per-backend cost waits
  for the RC8 numbers). `docs/reference/glossary.md` defines the terms the
  metric pages use, each entry naming the flag that controls it. The new
  `scripts/docs/check_glossary_links.py` (wired into
  `make docs-fragments-check`) fails when a known term has no glossary
  entry, when a metric page uses a term without linking its entry, and when
  a metric page does not link the glossary; all 28 metric pages now carry
  the link.
