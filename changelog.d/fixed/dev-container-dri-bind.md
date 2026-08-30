Replace fragile `/dev/dri/by-path` bind with whole `/dev/dri` directory bind in
`dev/docker-compose.yml`. The former bind-mount referenced PCI-address-named
symlinks (e.g. `pci-0000:01:00.0-card`) that change after any PCI
re-enumeration (reboot, suspend/resume, GPU hotplug), causing Docker to fail to
start the container. The whole-directory bind targets the stable devtmpfs
`/dev/dri` entry which is always present, and carries both leaf device nodes and
the `by-path/` symlink subtree that the Intel level-zero runtime needs to
discover Arc GPUs. ADR-0528.
