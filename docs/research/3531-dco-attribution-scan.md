# Research-3531: DCO attribution scan

The DCO check rejects model/tool commit identities and attribution lines in
commits and PR bodies before landing.
[ADR-3531](../adr/3531-dco-attribution-identity-refusal.md) records the policy
and alternatives. This digest records the corpus, matching surface, Unicode
handling, and a command that reruns the actual scanner helpers.

## Evidence and scope

The master snapshot is `20fa27ac625ee00d7efcc87a99a6669efa949023`, with 6,290
commits. One identity record means the author name/email and committer
name/email of one commit; it does not mean one distinct person. The repeated
identity scan returned **0 matches**. The body scan over **300 PR bodies**, PR
numbers 2906 through 3395 including bot PRs, also returned **0 matches** (corpus
SHA-256 `5a4d6f5c3cbf`, checked 2026-11-11T04:30:20Z).

| Corpus | Maximum UTF-8 bytes | Maximum `splitlines()` count | Headroom and enforced bounds |
| --- | ---: | ---: | --- |
| 6,290 master commit messages | 70,469 (`891b8213ec3f`) | 1,459 (`3be8b7029f7a`) | Four times each maximum: 281,876 bytes and 5,836 lines |
| 300 PR bodies | 58,966 (PR 3045) | 544 (PR 3041) | Four times each maximum: 235,864 bytes and 2,176 lines |

Both scans ran on 2026-11-11; the command prints the exact UTC check time. The
13 tokens equal the `BANNED_COAUTHORS` tuple of the standards engine's
`tools/planning/commit_msg_check.py` (file SHA-256 `b4cdca7d4283`, checked
2026-11-11). The engine's Go `bannedCoauthorsRegex` in
`internal/forge/commit_message.go` holds the 11 word tokens with word boundaries
and omits the e-mail and bot-marker entries, so the two engine lists differ from
each other; this PR does not change either.

## Reproduce

Export the 300-body input as a JSON array of objects containing `number` and
`body`. The corpus spans PR numbers 2906–3395. The `gh pr list` search has no
number-range qualifier (a `number:2906..3395` search returns nothing), so export
recent PRs with a configured forge login and keep the range in Python; the
second command writes the file the fingerprint above was taken from:

```bash
gh pr list --repo VMAFx/vmafx --state all --limit 1000 --json number,body > .workingdir/evidence/dco-pr-bodies-all.json
python3 -I -c 'import json, sys; d = json.load(open(sys.argv[1])); json.dump(sorted((p for p in d if 2906 <= p["number"] <= 3395), key=lambda p: p["number"]), open(sys.argv[2], "w"))' .workingdir/evidence/dco-pr-bodies-all.json .workingdir/evidence/dco-pr-bodies.json
```

Bodies can be edited after measurement. Keep the resulting JSON with the printed
SHA-256 fingerprint, and compare that fingerprint before treating a later export
as the same corpus. The command below requires 300 entries, reports the number
range and maximum sizes, and calls the production helpers rather than copying
the matching rules. Omit the JSON argument for the independently reproducible
master measurement. Run from the repository root:

```bash
CUDA_VISIBLE_DEVICES= python3 -B - .workingdir/evidence/dco-pr-bodies.json <<'PY'
import hashlib
import importlib.util
import json
import sys
from dataclasses import replace
from datetime import datetime, timezone
from pathlib import Path

from scripts.lib.safe_subprocess import run

root = Path.cwd()
spec = importlib.util.spec_from_file_location('dco_corpus', root / 'scripts/ci/check-dco.py')
assert spec is not None and spec.loader is not None
mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)
ref = '20fa27ac625ee00d7efcc87a99a6669efa949023'
result = run(['git', 'log', '-z', f'--format={mod.LOG_FORMAT}', ref],
             allowed_executables=['git'], capture_output=True, text=True, check=True,
             max_output_bytes=64 * 1024 * 1024)
fields = result.stdout.split('\0')
assert fields.pop() == '' and len(fields) % mod.LOG_FIELDS == 0
commits = [mod.Commit(*fields[i:i+mod.LOG_FIELDS]) for i in range(0, len(fields), mod.LOG_FIELDS)]
summary = {
    'checked_at': datetime.now(timezone.utc).isoformat(), 'master': ref,
    'identity_records': len(commits),
    'identity_hits': sum(mod.commit_attribution_reason(replace(c, message='')) is not None for c in commits),
    'largest_message_bytes': max(len(c.message.encode('utf-8')) for c in commits),
    'largest_message_lines': max(len(c.message.splitlines()) for c in commits),
}
if len(sys.argv) > 1:
    data = Path(sys.argv[1]).read_bytes()
    bodies = json.loads(data)
    assert len(bodies) == 300
    summary.update({
        'corpus_sha256': hashlib.sha256(data).hexdigest(), 'pr_bodies': len(bodies),
        'first_pr': min(p['number'] for p in bodies), 'last_pr': max(p['number'] for p in bodies),
        'body_hits': sum(bool(mod.pr_body_attribution(p['body'] or '')) for p in bodies),
        'largest_body_bytes': max(len((p['body'] or '').encode('utf-8')) for p in bodies),
        'largest_body_lines': max(len((p['body'] or '').splitlines()) for p in bodies),
    })
print(json.dumps(summary, indent=2))
PY
```

## Tokens and matching rationale

Identity matching checks case-insensitive substrings in author and committer
names and email addresses. The trailer scan uses the same token list, including
its bot marker. The list follows `tools/planning/commit_msg_check.py` in the
standards engine and must change with that source; it is not expanded by this
PR.

| Token | Why it is present |
| --- | --- |
| `claude` | Assistant/model family appearing in attribution identities |
| `anthropic` | Identity-domain marker in the inherited attribution policy |
| `openai` | Identity-domain marker in the inherited attribution policy |
| `chatgpt` | Assistant product attribution marker |
| `gpt` | Model-family marker, deliberately retaining substring semantics |
| `codex` | Coding assistant attribution marker |
| `gemini` | Assistant/model-family attribution marker |
| `copilot` | Coding assistant attribution marker |
| `cursor` | Coding editor attribution marker |
| `opencode` | Coding client attribution marker |
| `antigravity` | Coding client attribution marker |
| `noreply@anthropic.com` | Explicit historical tool address; retained even though another token overlaps it |
| `[bot]` | Bot co-author trailer marker; excluded from identity checks to retain legitimate bot authors |

No match in the measured corpus establishes compatibility with that corpus
alone. Substring matching can still reject a future human name or address.
Generated/created attribution phrases and the robot emoji are line rules
independent of the token list; ordinary mentions elsewhere in prose are not
identity fields. Attribution-looking quoted or fenced lines have no exemption.

## Unicode, parsing, and bounded input

For both commit messages and PR bodies, each line is normalized with NFKC before
matching. Leading whitespace and all Unicode format characters (category `Cf`)
are removed, including U+FEFF, U+200B through U+200D, and U+2060. Fullwidth
attribution words therefore receive the same checks as their ASCII forms. This
does not attempt unrestricted homoglyph detection or change identity substring
semantics.

Git log fields use `%x00`, and `git log -z` terminates records with NUL.
Embedded legacy U+001F or U+001E characters remain data and cannot move an
identity into another field. Git output is captured one commit at a time with an
explicit byte ceiling. Body files read at most their limit plus one byte; event
JSON has a separate ceiling of six times the body-byte limit plus 1 MiB metadata
to cover JSON escapes. Excess bytes or lines produce a refusal naming the bound,
never a truncated successful scan.

## Validation

The existing `scripts/ci/tests/test_check_dco.py` module contains
temporary-repository injection cases, Unicode-prefix and NFKC cases, fixture
event bodies, large multibyte event text, and exact-bound/bound-plus-one cases
for bytes and lines. Mutation runs restore each vulnerable path on the working
copy, require its new tests to fail, and restore the fix. Run only the named
module for focused validation:

```bash
CUDA_VISIBLE_DEVICES= praetorctl admit --label test -- python3 -B -m pytest -q -p no:cacheprovider -rs scripts/ci/tests/test_check_dco.py
```

Local Git 2.56.0 documentation
([`git-log(1)`](https://git-scm.com/docs/git-log#Documentation/git-log.txt--z),
literal-format and terminator sections) and a two-record capture validated
`%x00` and NUL termination. Python's installed
[`unicodedata.normalize`](https://docs.python.org/3/library/unicodedata.html#unicodedata.normalize)
and
[`unicodedata.category`](https://docs.python.org/3/library/unicodedata.html#unicodedata.category)
documentation validated the normalization/category APIs. Workflow body
extraction uses installed [jq 1.8.2's raw-output
option](https://jqlang.org/manual/v1.8/) and the existing squash check's
`--body-file` interface. No new dependency, action pin, or permission is
introduced.
