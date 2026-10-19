- **The RC3 home GPU retest kit survives a failing `nvidia-smi`.** `scripts/dev/rc3-home-gpu-retest.sh`
  wrote nvidia-smi's GPU list into `host.txt` as the last command of an `&&` list under `set -e`, so a
  failing nvidia-smi (exit 18 after a driver update that leaves the kernel module and the library at
  different versions) ended every run, `--dry-run` included. The kit now writes
  `nvidia-smi failed: exit N, no GPU inventory` into `host.txt` and goes on.
