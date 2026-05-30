- IDE and lint configs updated for the ADR-0700 `libvmaf/` → `core/`
  directory rename. Touches `.vscode/c_cpp_properties.json` and
  `.zed/settings.json` IntelliSense include paths, `.github/CODEOWNERS`
  review-routing globs, `.clang-tidy` `HeaderFilterRegex`, and
  `.dockerignore` + `.gitignore` build-tree / subprojects patterns. No
  source or build-system change — these files were missed in the
  original rename and stop matching real paths on disk; CODEOWNERS
  was routing to nothing, clang-tidy was filtering against a
  non-existent regex root, and ignore rules were leaking
  `core/subprojects/` extractions and `core/build*/` trees into
  status output and image layers. See ADR-0700. Note:
  `scripts/dev/project_modernization_audit.py` is intentionally NOT
  in this PR — already covered by in-flight PR #287.
