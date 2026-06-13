The MOS-head trainer now maps KonViD-150k score-drop split labels
(`k150ka` / `k150kb`) to the standard `train` / `val` vocabulary so
the canonical held-out boundary is honoured and the production-flip
gate fires against the researchers' intended partition instead of a
random k-fold split. (ADR-0455)
