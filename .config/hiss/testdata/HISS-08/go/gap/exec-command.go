package p

import "os/exec"

// Run reaches os/exec directly instead of the single audited exec entry point.
func Run(name string) error {
	return exec.Command(name).Run()
}
