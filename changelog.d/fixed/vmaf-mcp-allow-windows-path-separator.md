`VMAF_MCP_ALLOW` now uses `filepath.SplitList` (OS path-list separator) instead
of a hardcoded `":"` to split the colon/semicolon-delimited path list.
On Windows the list separator is `";"`, so entries with drive letters such as
`C:\foo` were previously silently mis-split at the colon.
Unix behaviour is unchanged (`":"` remains the list separator).
(ADR-1084)
