- Reject unpinned external container images that bypassed the central base-image
  guard through `FROM`, `COPY --from`, platform/other flags or instruction case.
  Preserve exact local image consumers and test mirror repair in isolated fixtures.
