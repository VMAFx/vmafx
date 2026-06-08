- `dev/Containerfile`: change vmaf user/group from GID/UID 1000 to 2000 —
  Ubuntu 26.04 (Resolute Raccoon) reserves GID/UID 1000 for the built-in
  `ubuntu` account, causing `groupadd --gid 1000 vmaf` to exit 4 and
  blocking every container rebuild. Also updated both BuildKit ccache-mount
  `uid=/gid=` directives to match. (ADR-1101)
