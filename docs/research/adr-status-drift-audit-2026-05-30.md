<!-- markdownlint-disable MD060 -->
# Research: ADR status-field drift audit (2026-05-30)

**Date**: 2026-05-30
**Scope**: All 630 ADRs under `docs/adr/[0-9]*-*.md`.
**Trigger**: Status fields are the single allowed mutation on Accepted
ADRs (per ADR-0028 / ADR-0106 immutability rule). Drift accumulates
because successor ADRs land without flipping the predecessor's Status,
or because authors put the supersedes relationship in the Status slot
instead of beside it.

## Method

1. Parse `**Status**: <value>` from every ADR's header block. Fall back
   to `## Status` h2-style header when the header-list form is absent.
2. Bucket each value into `Accepted | Proposed | Superseded | Deprecated |
   Draft | Missing | Other`.
3. Build two relations:
   - **claim_supersedes[Y] = [X, X, ...]** — every X whose body contains
     `Supersedes ADR-Y`.
   - **self_marked_superseded[Y] = X** — every Y whose Status field is
     `Superseded by ADR-X`.
4. **Drift = ADR Y where claim_supersedes[Y] is non-empty but Y's Status
   is still `Accepted`**.
5. **Orphan = ADR Y marked `Superseded` with no successor identified in
   either relation.**

## Findings

### Status counts (pre-sweep)

| Status     | Count |
|------------|-------|
| Accepted   | 569   |
| Proposed   | 31    |
| Missing    | 19    |
| Superseded | 7     |
| Other      | 4     |
| **Total**  | **630** |

### Drift detected (3 candidates)

| ADR | Claimed superseder(s) | Actual decision |
|---|---|---|
| ADR-0257 | ADR-0286 (partial) | KEEP `Accepted` — ADR-0286 only supersedes ADR-0257's "Negative consequence about content-independent saliency"; the MobileSal real-weights deferral itself still stands. |
| ADR-0537 | ADR-0552, ADR-0566 (clause-only) | KEEP `Accepted` — ADR-0566 explicitly states it supersedes "ADR-0537 (the 'places=3 acceptable as follow-up' clause only)"; the five kernel-crash fixes that are the body of ADR-0537 remain operative. |
| ADR-0573 | ADR-0738 (full) | **FLIP to `Superseded by ADR-0738`** — ADR-0738 §References explicitly notes "Supersedes ADR-0573" without scope qualifier; CUDA 13.2 → 13.3 container pin is a clean replacement. |

### Status-field malformation (3 cases)

ADR-0105 / ADR-0106 / ADR-0107 each have:

- **Status**: Supersedes [ADR-00NN](00NN-…)

This is malformed — the Status field encodes the ADR's own state, not
its relationship to a predecessor. The successor is itself `Accepted`
and active; the `Supersedes` relationship belongs in a sibling field.

**Fix**: split into two lines —

- **Status**: Accepted
- **Supersedes**: [ADR-00NN](00NN-…)

### Orphan Superseded chains

None. Every ADR currently marked `Superseded` has a successor identified.

### Out-of-scope observations (noted for later)

- 19 ADRs have no `Status` field at all (e.g. ADR-0256, ADR-0313,
  ADR-0352, ADR-0357, ADR-0359, ADR-0362, ADR-0370, ADR-0371, ADR-0416,
  ADR-0418, ADR-0460, ADR-0561, ADR-0566, ADR-0584, ADR-0747, ADR-0752,
  ADR-0754, ADR-0757, ADR-0860). Most predate the field-required
  template. Backfilling them is a separate sweep (filed as backlog
  candidate); none claim to be superseded by another ADR.
- ADR-0388 has `Status: Draft` (non-canonical). The header also has
  `Deciders: TBD`, so it really is mid-flight — left untouched.
- `docs/adr/README.md` indexes only 487 of 630 ADRs (large gap).
  Independent of status drift; not in scope here.

## Decision matrix (changes applied this PR)

| Change | Pros | Cons | Chosen |
|---|---|---|---|
| Flip ADR-0573 to `Superseded by ADR-0738` | Honours the only unambiguous full-supersede chain; satisfies the "Status is the one allowed mutation" rule | None | YES |
| Flip ADR-0257 / ADR-0537 to `Superseded` | Mechanically symmetric with the body-claim scan | Materially wrong — the bulk of each ADR's decision is still operative; partial-clause supersedes are not full supersedes | NO |
| Normalise ADR-0105 / 0106 / 0107 Status to `Accepted` + new `Supersedes` field | Removes the malformed Status; surfaces the Supersedes relationship without losing it | Mildly extends the header schema; harmless | YES |
| Sweep the 19 missing-Status ADRs | Would close the gap | Separate concern (no successor claims), risks miscategorising mid-flight ADRs | NO — file as separate backlog |
| Backfill `docs/adr/README.md` to all 630 ADRs | Restores discoverability | Massive diff, separate concern | NO — out of scope |

## Reproducer

```bash
# Re-run the audit against any commit:
python3 <<'EOF'
import re, glob, os
from collections import defaultdict
files = sorted(f for f in glob.glob('docs/adr/[0-9]*-*.md')
               if not f.endswith('0000-template.md'))
status_re = re.compile(r'(?im)^\s*[-*]?\s*\*\*Status\*\*\s*[:：]\s*(.+?)\s*$')
sups_re   = re.compile(r'(?i)Supersedes\s+ADR[-_]?(\d{4})')
records, claim = {}, defaultdict(list)
for f in files:
    num = int(re.match(r'.*?(\d{4})-', os.path.basename(f)).group(1))
    c = open(f).read()
    m = status_re.search(c)
    records[num] = (m.group(1).strip() if m else None, c)
    for y in sups_re.findall(c): claim[int(y)].append(num)
for y, claimers in sorted(claim.items()):
    raw = records.get(y, (None,))[0]
    if raw and 'accepted' in raw.lower():
        print(f"DRIFT: ADR-{y:04d} Accepted but claimed superseded by {claimers}")
EOF
```

## Consequences

- **Positive**: ADR index now correctly reflects what's live vs.
  retired for the three unambiguous cases. Future agents reading
  ADR-0573 will follow the pointer to ADR-0738 instead of acting on
  the older CUDA 13.2 pin.
- **Neutral**: Two false-positive drifts (ADR-0257, ADR-0537) get
  explicit documentation here so the next audit doesn't re-flag them.
- **Negative**: The 19 missing-Status and ADR-0388 Draft outliers
  remain. Tracked as follow-ups, not addressed here to keep the PR
  scoped to actual drift.
