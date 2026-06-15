- **Renovate config:** fixed an invalid schedule (`before 6am on weekdays` →
  `before 6am every weekday`) — Renovate's later.js parser rejects `on weekday(s)`
  and had stopped all dependency PRs. `every weekday` is the form Renovate's own
  `schedule:weekdays` preset uses.
