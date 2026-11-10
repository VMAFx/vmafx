- **Every README badge but the Praetor-managed governance badge now has the same `for-the-badge` design, and the status badges come from this project's own render.**
  `.github/workflows/readme-badges.yml` draws the seven workflow badges and the OpenSSF Scorecard,
  OpenSSF Best Practices and Codacy badges every 15 minutes onto the `badges` branch: a workflow badge
  shows the newest finished master push run, so a run in progress never hides a red one, and the
  Scorecard badge now shows the score. See `docs/development/readme-badges.md` (ADR-3519).
