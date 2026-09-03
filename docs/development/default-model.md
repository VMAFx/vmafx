<!-- markdownlint-disable MD013 -->
# Changing the default VMAF model

When a caller names no model, the fork scores with one default. This page is
the operational guide to that default: where it is defined, how to change it,
and what the change is gated on.

The design rationale is in
[ADR-1168](../adr/1168-default-model-single-source.md).

## Where it is defined

One place:

```c
/* core/include/libvmaf/model.h */
#define VMAF_DEFAULT_MODEL_VERSION "vmaf_v0.6.1"
```

Everything else derives from it.

| Consumer | How it reads the default |
| --- | --- |
| C / C++ compiled against the headers | the `VMAF_DEFAULT_MODEL_VERSION` macro |
| Anything linking libvmaf at runtime, including bindings | `vmaf_default_model_version()` |
| Go | `pkg/model.DefaultVersion` (mirror) |
| `vmaf-tune` | `vmaftune.defaultmodel.DEFAULT_MODEL` (mirror) |
| `vmaf-roi-score` | `vmafroiscore.defaultmodel.DEFAULT_MODEL` (mirror) |

The three mirrors exist because those components deliberately do not link
libvmaf — forcing cgo on `vmafx-tune`, `pkg/fast`, `pkg/bisect` and `pkg/corpus`,
or a C extension on the two Python tools, purely to learn a string would make
them unbuildable without the C library. They cannot drift: the gate below
compares each one against the header on every `make lint` and every commit.

## How to change it

1. Edit the macro in `core/include/libvmaf/model.h`. That is the change.
2. Run the gate. It will name every mirror that now disagrees:

   ```bash
   bash scripts/ci/check-default-model-single-source.sh
   ```

3. Update the mirrors it names, to match the header. Never the other way round.
4. Re-run the gate until it passes, then run the golden gate (below).

## What it is gated on

`scripts/ci/check-default-model-single-source.sh` runs in `make lint` and as a
pre-commit hook. It fails when:

- the header does not define `VMAF_DEFAULT_MODEL_VERSION`, or defines it twice;
- a mirror constant disagrees with the header;
- any component reintroduces its own hardcoded fallback model name.

It is itself tested, in both directions, by
`scripts/ci/tests/test-default-model-single-source.sh` (22 cases).

The gate matches enumerated *fallback spellings* rather than the bare literal,
because almost every occurrence of `"vmaf_v0.6.1"` in the tree is a doc-comment
or a model-name lookup table and banning those would make the gate unusable.
The forms it knows are: assignment, a `key: value` default, `return`,
`getattr(x, y, "...")`, `.get(..., "...")`, `or "..."`, `||  "..."`, and Go
flag registrations (`StringVar` / `String` / `StringP`). Comments are ignored.

There is deliberately **no** "is this a comment?" heuristic. An earlier version
had one and it was worse than the problem it solved: loosely anchored, it
classified `const char *model = "vmaf_v0.6.1";` as a comment — the `*` of an
ordinary pointer declaration — and blinded the gate to the most idiomatic C
spelling of a hardcoded default. Tightly anchored, it still swallowed
`*dest = "..."` and `#define FALLBACK ...`, and it could never handle prose
inside a Python docstring. Instead, every pattern requires real
assignment / return / call syntax immediately around the literal, which prose
does not have. There are test cases for each of those, in both directions.

Two spellings are knowingly **not** caught, because `git grep` is line-oriented
and neither is something a contributor writes by accident: a literal split
across lines, and one built by concatenation (`"vmaf_v0" + ".6.1"`).

**If you invent a new way to spell "fall back to a literal model", add it to
both the gate and its test.** Enumeration can miss a spelling. Adversarial
review of the first implementation found `getattr(args, attr, "vmaf_v0.6.1")`
shipping in `vmaftune/cli.py`, invisible to the original patterns; a second
review found the comment-filter flaw above and three more idioms
(a Python ternary, a C ternary, and `os.getenv`).

### Pinning a model on purpose

Some code must name a specific model regardless of the fork's default. Mark
those lines so the reason lives next to the code:

```c
.version = "vmaf_v0.6.1", /* vmaf-model-pin: AOM CTC v1.0 mandates this exact model */
```

The AOM CTC preset in `core/tools/cli_parse.cpp` is the standing example: the
Common Test Conditions specification requires that exact model, so it must
*not* follow the fork's default. Test fixtures, the Netflix golden harness, the
built-in model registry in `core/src/model.c` and the documentation tree are
exempt by path.

## The constraint on actually changing the value

Changing the default changes the score every user gets when they do not pass
`--model`. One Netflix golden assertion pins that number directly:

```text
python/test/vmafexec_test.py::VmafexecQualityRunnerTest
    ::test_run_vmafexec_runner_use_default_built_in_model
```

It passes `use_default_built_in_model: True` and then asserts the resulting
VMAF and per-feature scores. It exists precisely to pin the default model's
output, so **any** change of default breaks it — and
[ADR-0024](../adr/0024-netflix-golden-preserved.md) forbids editing Netflix
golden assertions.

Measured on the standard 576x324 golden pair:

| Default model | VMAF (mean) |
| --- | --- |
| `vmaf_v0.6.1` | 76.667831 |
| `vmaf_v1.0.16_3d0h` | 82.816059 |

With the value left at `vmaf_v0.6.1`, all 271 golden tests pass. Switching to
`vmaf_v1.0.16_3d0h` fails exactly that one test and no other.

So the mechanism and the value are separate decisions. The mechanism is settled
(ADR-1168). Moving the value to the v1.0.16 line requires first resolving how
that single golden assertion is treated, which is a maintainer decision, not an
implementation detail.

Always run the golden gate after changing the value:

```bash
make test-netflix-golden
```

If your virtualenv lacks pytest, run the five files directly:

```bash
PYTHONPATH=$PWD/python CUDA_VISIBLE_DEVICES= python3 -m pytest \
    python/test/quality_runner_test.py \
    python/test/feature_extractor_test.py \
    python/test/vmafexec_test.py \
    python/test/vmafexec_feature_extractor_test.py \
    python/test/result_test.py -q -m "not slow"
```

## Which model would be next

[`docs/models/v1.md`](../models/v1.md) identifies `vmaf_v1.0.16_3d0h` as the
standard 1080p model at a 3H viewing distance — the direct counterpart to
`vmaf_v0.6.1`, which was trained for the same condition. The other v1.0.16
variants target different viewing conditions (`5d0h` phone, `1d5h_2160` 4K at
1.5H, `3d0h_2160` 4K at 3H on a [0, 110] range) and are not general-purpose
defaults.
