- Restore `feature_collector.cpp` as the single production and test implementation,
  preserving the mutex, TSan, lifetime, and error-contract fixes that had existed
  only in the resurrected C twin.
