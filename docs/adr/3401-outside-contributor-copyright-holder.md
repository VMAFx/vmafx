<!-- markdownlint-disable MD013 MD060 -->

# ADR-3401: Outside contributors hold copyright in the files they write

- **Status**: Accepted
- **Date**: 2026-11-06
- **Deciders**: lusoris
- **Tags**: `license`, `docs`, `governance`

## Context

[ADR-0861](0861-vmafx-copyright-policy-drop-anthropic.md) set the single notice
`Copyright 2026 Lusoris` for wholly-new fork files when only the maintainer and agents
acting for the maintainer wrote them. Outside contributors now submit new files through
pull requests from their forks. A copyright line asserts a rights claim; the maintainer
holds no rights in a file someone else wrote.

The AI provenance check also treats human names such as Claude Martin and Gemini Okafor
as AI product names. Sharing a word with an AI product does not make a human holder invalid.

## Decision

- A new file written by an outside contributor carries `Copyright <year> <contributor name>`
  and an `SPDX-License-Identifier` line.
- A new file written by the maintainer, or by agents acting for the maintainer, keeps
  `Copyright <year> Lusoris` and its SPDX licence line.
- Upstream-derived files keep the Netflix header. Edits to an existing file leave its
  copyright line as it is.
- ADR-0861's single-notice decision applies to maintainer-written files. Its removal of AI
  vendors from copyright lines stays in force. ADR-0861 remains unchanged as an accepted
  historical record.
- The provenance check accepts human names that share a word with an AI product.
  `Anthropic`, `OpenAI`, `ChatGPT` and `Copilot` remain forbidden. `Claude`, `Gemini` and
  `Codex` are forbidden when followed by the end of the line, punctuation, or a product
  word (`Code`, `CLI`, `AI`, `Assist`, `Sonnet`, `Opus`, `Haiku`, `Fable`, `Pro`, `Flash`),
  or when adjacent to an unambiguous vendor name. A following capitalised surname, as in
  `Claude Martin`, is accepted.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep Lusoris on every new file | One header template for all new files. | Claims rights the maintainer does not hold. | Rejected: the notice must identify the actual holder. |
| Use dual contributor and Lusoris notices | Names both the contributor and the maintainer. | Still claims rights the maintainer does not hold; ADR-0861 already rejected dual notices. | Rejected: adding the contributor does not cure the overclaim. |
| Omit per-file copyright lines for outside files | Avoids naming the wrong holder. | Loses the per-file marker ADR-0861 retained. | Rejected: retain the marker with the contributor's name. |

## Consequences

- Outside contributors can identify themselves as holders without granting the maintainer
  rights through a misleading notice. [CONTRIBUTING.md](../../CONTRIBUTING.md) links this
  scope clarification alongside ADR-0861.
- Maintainer-written and upstream-derived files retain their existing header policy;
  editing a file does not replace its holder.
- `scripts/dev/relicense_fork_files.py` distinguishes ambiguous human names from AI product
  usage. Positive human-name cases and planted AI-product cases in
  `scripts/dev/tests/test_relicense_fork_files.py` cover the distinction.

## References

- [ADR-0861](0861-vmafx-copyright-policy-drop-anthropic.md): single maintainer notice and
  removal of AI vendors from copyright lines.
- [ADR-0025](0025-copyright-handling-dual-notice.md): historical
  per-file copyright handling.
- [CONTRIBUTING.md](../../CONTRIBUTING.md): contributor licence-header requirements.
- Maintainer ruling, 2026-11-06, project decision ledger id
  `vmafx-3317-copyright-holder` (paraphrased): outside contributors name themselves as
  holders; the maintainer's name applies to files written by or for the maintainer.
