- **The Python hash locks are regenerated with uv 0.13.1.** Every lock still pins `aiohttp` 3.14.5,
  but the repository dependency graph kept listing 3.14.3 from an older parse, so Dependabot alerts
  for the `aiohttp` advisories fixed in 3.14.4 and 3.14.5 stayed open. The refresh rewrites each lock
  from its manifest entry and moves eleven transitive pins forward (among them `certifi`, `starlette`,
  `setuptools`, `filelock` and `virtualenv`); `mcp` stays at 1.29.0 under its declared exception.
