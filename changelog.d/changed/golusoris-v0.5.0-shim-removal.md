- **Removed the golusoris interim shims now obsolete on v0.5.0.** The pin bump to
  v0.5.0 integrated golusoris#234 (logger reads `log.level` from the prefixed
  config) and #227 (operator `ctrl.SetLogger` + `Options.WebhookPort`/
  `WebhookHost`), so the per-binary workarounds are gone: the
  `VMAFX_LOG_LEVEL`→`LOG_LEVEL` env bridges (server/node/operator), the operator
  `setupCtrlLogger` shim and config-read webhook gate (now `opts.WebhookPort`),
  and the `vmafx-tune` `levelledLogger` fx decorator. Behaviour is unchanged —
  `VMAFX_LOG_LEVEL` and the operator webhook port still bind through the `VMAFX_`
  prefix natively. Verified: all binaries build + test on v0.5.0 with the log
  level honored and the operator webhook bound by the framework.
