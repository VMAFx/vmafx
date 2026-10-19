<!-- markdownlint-disable MD013 -->
# Support VMAFx

VMAFx is free and open source, and it stays that way. If it saves you time,
you can sponsor it from $5 a month. Sponsors are thanked on this page and in
the repository; sponsoring buys recognition only, with no paid support, no
feature voting and no paywall.

## What the money pays for

- **CI time.** Every change runs the build matrix, the numerical-exactness
  gates and the sanitizer jobs. This is the largest running cost.
- **Cloud GPU test time.** VMAFx ships CUDA, SYCL, HIP and Metal backends that
  must give the same scores as the CPU. Checking that on hardware the project
  does not own is paid by the hour.
- **An AI workstation (next goal).** Training and evaluating the tiny quality
  models needs a machine of its own. This comes after the first goal.

The first goal is **$250 a month** for CI and cloud GPU time.

## How to sponsor

| Route | Currency | Status |
| --- | --- | --- |
| [GitHub Sponsors](https://github.com/sponsors/lusoris) | USD (GitHub takes dollars only) | Live |
| [Ko-fi](https://ko-fi.com/lusoris) | EUR | Live |
| [Patreon](https://www.patreon.com/Lusoris) | EUR, the same four tiers at 5, 25, 100 and 500 euros | Live |

The GitHub profile belongs to the maintainer's personal account (`lusoris`)
and covers VMAFx and the maintainer's related open-source work. GitHub
Sponsors also accepts custom amounts.

## Tiers

--8<-- "SPONSORS.md:tiers"

## What recognition means

- Names, links and logos appear only for sponsors, and only at the placement
  the tier states. Say so if you would rather stay anonymous.
- Logos are placed by the maintainer after you send them. The full rules are
  in [`SPONSORS.md`](https://github.com/VMAFx/vmafx/blob/master/SPONSORS.md).
- Sponsoring gives no influence over the roadmap, no early access and no
  support. Questions and bugs go through the normal issue tracker for everyone.
- The decision and the alternatives are recorded in
  [ADR-2689](adr/2689-sponsorship-tiers-recognition-only.md).

## Our sponsors

--8<-- "SPONSORS.md:recognised"

Thank you to everyone who sponsors, and to everyone who reports bugs, sends
patches or tests on their own hardware. Both kinds of help matter.
