- **The dev container runs the command it is given and exits.** `dev/scripts/dev-mcp-entrypoint.sh`
  ignored its arguments and always kept the container running, so every
  `docker run vmaf-dev-mcp:local <command>` left a container (and its healthcheck) up. A start with
  a command now runs it and exits with its status; a start without one still stays up for
  `docker exec`. The `smoke-probe-cron` compose service now runs its probe loop.
  `docs/development/dev-mcp.md` shows the one-shot forms.
