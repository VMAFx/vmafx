- **The controller no longer writes its job queue into the working directory.**
  With `VMAFX_DB_PATH` unset, `vmafx-controller` opened `vmafx-controller.db`
  relative to the directory it started in, which is how three queue files ended
  up committed under `cmd/vmafx-controller/`. The default is now
  `vmafx/vmafx-controller.db` under the user's state directory
  (`$XDG_STATE_HOME`, else `~/.local/state`; the user configuration directory
  on macOS and Windows), created with mode 0700; with no such directory the
  controller refuses to start and names `VMAFX_DB_PATH`. The image and the Helm
  chart set `/data/vmafx-controller.db` and are unchanged. The committed files
  are removed and ignored. See [the controller guide](docs/server/controller.md#configuration).
