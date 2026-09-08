# Research-2055: OpenSSF passing evidence and truthful support policy

## Scope and authoritative sources

The user requested fixes for Scorecard gaps. OpenSSF's passing badge is a
separate, voluntary self-certification program; writing documentation does not
create a passing badge. This assessment reads the
[official passing criteria](https://www.bestpractices.dev/en/criteria/0),
[achievement rules](https://www.bestpractices.dev/en/criteria_discussion#achieving-a-badge)
and BadgeApp source at `33907f3e0f8748abbeb587af3a45ed9f24deb380`.
The official `criteria/criteria.yml` contains 67 passing identifiers, categories
and justification/N/A flags. The local worksheet preserves all 67 exactly;
criterion content is attributed to the OpenSSF Best Practices badge contributors
under their CC-BY-3.0+ terms.

The review candidate is `76a7c467c3524478c25d588a31b37b2d45d96081`.
Live public master was `78c9d2bfc580880919d5168c18ad19441f9db96a`.
Separate source-bound proposals use only public-master evidence; pending
SECURITY, contribution-policy and RC1 changes are not submitted as landed facts.
The local labels Evidence/Prepared/Unknown/Gap are not BadgeApp statuses and
are not automatically converted to Met answers.

## Verified discrepancies and correction

The active repository's releases API returned an empty list, yet SECURITY
promised the latest two VMAFx 3.x releases and completed provenance/signature
coverage. VMAFx instead plans its first 1.0 release candidate. Correct support
and verification prose without inventing a release or treating upstream tags
as this project's supported versions. GitHub's private-reporting API initially
returned disabled while the policy directed reporters there; enabling and
verifying that setting was an owner-side action, separate from this patch. A
subsequent independent API readback confirmed `enabled: true`; this establishes
the live channel, not response history.

The owner created [project 14549](https://www.bestpractices.dev/en/projects/14549).
Its initial public JSON identified the correct repository and an in-progress
badge; unsaved UI automation proposals were not persisted answers. No passing
status or completion percentage is claimed here. Account permissions, data
licensing and final submission remain owner decisions; this task made no
external project edits.

The current policy now requires publicly known medium-or-higher issues to be
patched and the fix released within 60 days. Interim mitigation alone does not
satisfy the official criterion. This expresses the requested
current policy, not verified historical compliance. CONTRIBUTING explicitly
requires automated tests with major new functionality. Both need publication
before being cited as current-master policy evidence.

## API/schema check

The [API guide](https://github.com/ossf/best-practices-badge/blob/33907f3e0f8748abbeb587af3a45ed9f24deb380/docs/api.md)
requires a `.json` URL suffix for JSON data; the Accept header is not its format
selector. The [OpenAPI file](https://github.com/ossf/best-practices-badge/blob/33907f3e0f8748abbeb587af3a45ed9f24deb380/best_practices.openapi.yaml)
describes read routes and only a minimal Project shape, not a complete write
schema. The source/API guide use `<criterion>_status` and paired justification
fields with external `?`, `Unmet`, `N/A`, `Met` values. The
[proposal-file documentation](https://github.com/ossf/best-practices-badge/blob/33907f3e0f8748abbeb587af3a45ed9f24deb380/docs/bestpractices-json.md)
explicitly ignores unknown answers. No guessed write payload or affirmative
bulk auto-fill file is added to this repository.

## Validation and limits

The assessment has one row for every official passing identifier, with no
omissions or duplicates. Local links and documentation are checked with the
normal documentation hooks; no prose-mirroring software tests or native rebuild
are needed. Source/API snapshots, exact public-master evidence bindings and
review-only field proposals are retained under
`.workingdir2/cache/scorecard-badge-evidence-20260908/`; canonical results are in
`.workingdir2/evidence/scorecard-badge-evidence-2026-09-08/`.

Developer knowledge, private-report response times, vulnerability age/closure,
credential validity and complete crypto coverage are not established by this
bounded review. Existing diagnostic baselines and release configuration do not
prove full release acceptance. No Netflix golden assertion, native source,
public API or FFmpeg surface changes.

No alternative implementation is needed to correct unsupported factual claims;
the Scorecard gate policy is handled separately. The human guide, root agent
invariant, rebase note, state entry and changelog fragment carry the associated
deliverables. Recheck the worksheet whenever public evidence or official criteria
change instead of treating this dated snapshot as permanent certification.

## GitHub Pages entrypoint follow-up

The user selected GitHub Pages as the project website. Its homepage returned
HTTP 200 but introduced itself only as a documentation overview, without a
concise product purpose or direct reporting/contribution links. The source now
adds that introduction while preserving the entire topic index. The worksheet
uses published Pages URLs alongside source proofs for basic and interface
documentation; `description_good` and `interact` stay prepared until the new
homepage text is deployed and read back. HTTP availability alone does not prove
a criterion or deploy a source change.

The bounded receipt in `.workingdir2/cache/scorecard-pages-intro-20260908/`
retains the live page responses, topic-index identity, changed-link checks and
normal documentation hooks. No new ADR is needed for this factual entrypoint
correction; no native test or numerical data changed.
