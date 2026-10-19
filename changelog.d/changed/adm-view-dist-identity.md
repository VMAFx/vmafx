- **The ADM viewing-distance merge compares configured values for identity
  through one helper.** `adm_view_dist.c` decides whether two `adm`
  registrations can share one extractor by comparing their configured
  distances; it now does so through `same_view_dist()`, which compares the
  values' bits. Option parsing refuses NaN and the distances are positive, so
  this is the same rule as before and no registration merges differently. The
  change clears two CodeQL `cpp/equality-on-floats` alerts.
