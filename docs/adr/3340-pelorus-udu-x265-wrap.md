<!-- markdownlint-disable MD013 MD041 MD060 -->
# ADR-3340: Pelorus side data behind the x265 UUID is read through an allow-list

- **Status**: Accepted
- **Date**: 2026-10-18
- **Deciders**: Lusoris
- **Tags**: pelorus, ffmpeg, api, interop

## Context

`vmaf_set_perceptual_sidedata()` receives a user data unregistered SEI payload:
16 bytes of `uuid_iso_iec_11578` followed by the SEI's data. FFmpeg patch
`0017` passes the payload unchanged. The reader accepted it when the first 16
bytes were the Pelorus blob UUID or the Pelorus carrier UUID
([ADR-1113](1113-vendor-pelorus-interop-abi.md), PR #2846).

libx265 writes its own UUID (`2ca2de09-b517-47db-bb55-a4fe7fc2fc4e`) in front
of every user data unregistered payload a caller hands it. A Pelorus blob that
went through libx265 therefore arrives as x265 UUID, Pelorus UUID, image. The
reader saw an unknown outer UUID, returned `-ENOENT`, and scored the frame
unweighted.

Pelorus classifies these payloads in its ADR-0210 and ships the vectors it
replays as `ffmpeg-patches/test/fixtures/udu-wrap-vectors.txt` (VMAFx/pelorus
`7f6bd706`).

## Decision

The reader classifies the payload before unwrapping it, with
`vmaf_pelorus_udu_classify()` in `core/src/feature/pelorus_udu_wrap.h`:

- **direct**: the payload starts with the blob or carrier UUID; it is unwrapped
  as before;
- **x265-wrapped**: the payload starts with the x265 UUID, carries more than 16
  data bytes, and the data starts with the blob or carrier UUID; the reader
  drops the outer 16 bytes and unwraps the rest;
- **none**: everything else, including any other outer UUID in front of a
  Pelorus UUID, a second x265 UUID, and a Pelorus UUID at any offset other than
  0 or 16. The payload stays foreign (`-ENOENT`).

The classifier is an allow-list of one outer UUID. It never skips 16 bytes and
tries again on an unknown UUID. The vector file is copied byte for byte to
`testdata/pelorus/udu-wrap-vectors.txt` (git blob `3a115ee3`), and
`test_pelorus_udu_wrap` replays it. `test_perceptual_weight` checks that a
wrapped blob and a wrapped carrier give the direct form's weight bit for bit,
and that an unknown outer UUID and a double x265 wrap stay foreign.

The Pelorus mirror files (`pel_blob_unwrap()` and the headers) stay
byte-identical to Pelorus; the classifier is fork code in `core/src/feature/`.

## Alternatives considered

| Option | Pros | Cons | Verdict |
| --- | --- | --- | --- |
| Allow-list the x265 UUID in the fork's reader (chosen) | Same rule as Pelorus' own reader; mirror files stay byte-identical; unknown wrappers stay foreign | One more fork header that follows Pelorus | Chosen |
| Strip the x265 UUID in FFmpeg patch `0017` | Library unchanged | API callers outside FFmpeg (GStreamer, the VMAFx API) still see the wrapped payload; two readers with different rules | Rejected |
| Skip any 16-byte outer UUID and retry | Accepts every future wrapper | Reads Pelorus bytes inside foreign SEIs; Pelorus ADR-0210 refuses it | Rejected |
| Add the classifier to the Pelorus mirror (`pel_blob_unwrap()`) | One place for both repositories | Mirror files are byte-locked to the Pelorus pin (ADR-1113); the change belongs to Pelorus first | Rejected for this repository |

## Consequences

- Frames that libx265 encoded with Pelorus side data are now weighted. A
  stream whose producer wrote another outer UUID still scores unweighted.
- A change to Pelorus' vector file is copied to `testdata/pelorus/` with its new
  blob id; the replay fails on fewer vectors than the copy holds or on a class
  without one.
- No public symbol changes; the return values of `vmaf_set_perceptual_sidedata()`
  keep their meaning.

## References

- Pelorus ADR-0210 (classification of x265-wrapped payloads) and
  `ffmpeg-patches/test/fixtures/udu-wrap-vectors.txt` at VMAFx/pelorus `7f6bd706`.
- Controller order `vmafx-udu-wrap-vectors-replay-20261018` (paraphrased: replay
  the Pelorus vectors against the fork's reader; vectors pass and an unknown outer
  UUID is refused).
- [ADR-1113](1113-vendor-pelorus-interop-abi.md), PR #2846 (carrier form).
