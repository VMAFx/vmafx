- **A pull request body copied from the template can opt out of the `docs/state.md` gate.** The
  template's example `no state delta: REASON` sits inside its HTML comment now, and the gate accepts
  a real opt-out even when the placeholder appears elsewhere in the body; before, the placeholder in
  the template's checkbox made every such body fail.
