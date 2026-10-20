- Setup: `scripts/setup/ubuntu.sh` installs its packages when run as root.
  With no `sudo` prefix, `DEBIAN_FRONTEND=noninteractive` became the command
  name and the install stopped with "command not found"; the variable now goes
  through `env`. The header names the 26.04 codename correctly (Resolute).
