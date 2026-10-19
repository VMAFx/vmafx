## Server workloads without VMAFX_BACKEND, unused chart helpers removed (2026-10-08)

`rc4/api-wp17-cleanup`, [ADR-2350](adr/2350-cloud-native-platform.md) D13.
The server's Deployment, StatefulSet and Job render `env` only from
`.Values.env`; no `[[chart_env]]` entry targets them, and the `unread`
escape of `[[chart_env]]` is gone, so the generator refuses an entry the
workload's binary does not read. `vmafx.podSpec`, `vmafx.containerSpec`,
`vmafx.volumes` and `templates/sidecar-trainer.yaml` are deleted;
`cmd/vmafx-mcp/main.go` has no `LOG_LEVEL` / `LOG_FORMAT` copy. A sync that
brings any of them back drops it again. `test_helm_config_env.py` guards the
server containers. no upstream file.
