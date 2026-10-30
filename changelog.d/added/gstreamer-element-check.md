- **The GStreamer element is built and tested in CI (ADR-3390).** The required
  check `GStreamer Element` builds the element on the commit's library, holds
  its installed properties to `core/api/vmafx.toml` and its scores to the CLI;
  it runs when the element, the library or the definition changes (#2474).
