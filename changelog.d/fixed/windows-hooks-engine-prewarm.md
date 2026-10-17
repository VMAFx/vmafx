- **ci**: the Windows Lefthook Pre-Commit job installs the pinned praetor
  engine once before the hooks run in parallel; parallel first-use installs
  replaced the binary another hook was running and failed the job at random
  (exit 126 or 127).
