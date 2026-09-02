- `clang-format` pinned to **v23.1.0** (was v22.1.5), completing the pre-commit
  hook refresh.
- **No reformat.** Every one of the 763 in-scope C/C++/CUDA files is already
  byte-identical to v23.1.0's output, verified by piping each through the new
  binary and `cmp`-ing against the tree. This needs no `.git-blame-ignore-revs`
  entry, contrary to the earlier planning note that assumed a major
  clang-format bump implies a tree-wide reformat — that assumption was never
  measured, and it is wrong here.
