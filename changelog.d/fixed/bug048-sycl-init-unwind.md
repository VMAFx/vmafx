- Restored deterministic cleanup on every still-unprotected SYCL feature-extractor
  initialization failure. Twelve translation units now release partially allocated
  USM, feature-name dictionaries, and failed graph registrations before returning;
  a device-free fault-injection gate covers all 13 affected extractor descriptors.
