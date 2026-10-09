//go:build apicompatgate

// SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
//
// SPDX-License-Identifier: EUPL-1.2

// Command gate is the Go API compatibility gate of the api:public-contract facet. The
// workflow .github/workflows/praetor-api.yml runs it on every pull request that is ready for
// review and on every push to the default branch; on a draft pull request it fails without
// running the gate, and a push to another branch or a tag starts no run:
//
//	go run tools/apicompat/gate/main.go [-base=<rev>] [-policy=auto|warn|reject] [-checker=<path>]
//
// It compares the exported API of every public Go module between a base revision and the
// commit checked out (HEAD) with the pinned checker (checkerPackage), run once per module from
// that module's directory against the repository root, so a nested module is compared as
// itself and never skipped by a root-only run.
//
// A module is a go.mod git tracks at the revision, outside the directories the go command
// skips in ./... patterns (testdata, vendor, and names starting with "." or "_"), whose module
// path has no internal element. A module at both revisions is compared, unless its path at HEAD
// is a later major version of its path at the base (isNewMajor): Go publishes a new major
// version under a new module path, so importers of the base path lose nothing, and the module is
// reported as a new major version instead. One only at HEAD is reported as added; one only at
// the base is a removed module, which is an incompatible change. More than maxModules modules,
// or a revision listing more than maxTreeEntries paths, fails the gate instead of comparing a
// truncated set.
//
// The checker builds every module with -mod=vendor when the repository root holds a vendor
// directory, although the go command vendors a nested module from that module's own directory.
// While the base or HEAD tracks a root vendor entry, nested modules are therefore compared in a
// temporary clone whose two commits drop it (vendorFreeClone); the root module is compared in
// the repository itself.
//
// -base defaults to the newest root release tag, vMAJOR.MINOR.PATCH with no directory prefix,
// merged into HEAD. A nested module's tag carries its directory (dir/v1.2.3) and is never
// selected. With neither -base nor such a tag there is no published API to compare, and the
// gate passes saying so.
//
// Compatibility policy and execution policy are separate. -policy=auto rejects incompatible
// changes once the newest root release tag merged into HEAD is v1 or later and reports them as
// warnings before that; warn and reject fix the policy. A comparison that did not run fails the
// gate whatever the policy: a git or discovery error, a malformed go.mod, a compared module
// whose packages do not build at HEAD, a checker that cannot be installed, that misses the
// removed function of a canary module (verifyChecker), that exits with any status but 0 or 1,
// or that prints an error.
//
// A module whose packages cannot be built even with the system packages the manifest declares
// (api.system_packages, installed by the workflow before this program runs) takes an exception
// from the manifest's exceptions list (rule api-compatibility, path <module dir>/go.mod). The
// workflow passes the entries in the environment variable APICOMPAT_EXCEPTIONS as a JSON list of
// {path, reason, expires} objects, because this program reads no manifest. While an entry holds
// (until the end of its expires day, UTC), the module is reported as not compared, with the
// entry's reason and expiry, and the gate goes on; once it expired, or with no entry, the
// build failure fails the gate as before. A module that builds is compared whatever the list
// says. The program never sets CGO_ENABLED.
//
// Exit status: 0 compatible, or incompatible under the warn policy; 1 incompatible under the
// reject policy; 2 the comparison did not run. The checker checks each revision out in the
// working tree, restores HEAD afterwards and refuses a dirty tree, so run the gate in a clean
// checkout.
//
// praetorctl adopt writes this file and praetorctl audit locks it: change it in Praetor's
// tools/apicompat, never in an adopted repository.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"time"
)

const (
	exitCompatible = 0
	exitRejected   = 1
	exitNotRun     = 2

	// checkerPackage is the checker, github.com/joelanford/go-apidiff, built from the pinned build
	// module (checkerGoMod, checkerGoSum, checkerTools) rather than installed as module@version.
	// The build module requires go-apidiff at commit c3e0953fa2fd of its main branch (2026-09-10),
	// the newest upstream commit, and golang.org/x/tools v0.51.0 exactly. Its newest release,
	// v0.8.3, builds against x/tools v0.33.0 and the commit against v0.49.0, which reads export
	// data up to version 4; Go 1.27.2 writes version 5, so either checker reads every package as
	// empty and passes every module (#1049). The CLI is "go-apidiff <oldCommit> [newCommit]
	// --repo-path=<root>": it loads ./... from its working directory and exits 0 when
	// compatible, 1 when it found an incompatible change and 2 when the comparison failed. A
	// flag or argument error is printed with cobra's "Error: " prefix while the status stays 0,
	// which classifyChecker reads as a failed run.
	checkerPackage = "github.com/joelanford/go-apidiff"
	checkerBinary  = "go-apidiff"

	maxTreeEntries   = 1 << 21
	maxModules       = 512
	maxTags          = 1 << 16
	maxRecordBytes   = 1 << 16
	maxGoModBytes    = 1 << 20
	maxGoModLines    = 1 << 16
	maxPathElements  = 256
	maxDiagnostics   = 1 << 20
	maxRevisionBytes = 4096

	// exceptionsEnv carries the manifest's api-compatibility exceptions, as the workflow renders
	// them (tools/apicompat/render.go).
	exceptionsEnv  = "APICOMPAT_EXCEPTIONS"
	maxExceptions  = 1024
	exceptionsDate = "2006-01-02"

	runTimeout     = 3 * time.Hour
	gitTimeout     = 2 * time.Minute
	installTimeout = 10 * time.Minute
	moduleTimeout  = 30 * time.Minute
)

// The checker's build module, written to a temporary directory by buildChecker. It is the text
// of a go.mod, go.sum and tools.go: go.mod requires go-apidiff and golang.org/x/tools exactly,
// tools.go imports both so "go mod tidy" keeps them required directly, and go.sum pins the
// closure. The text lives here, not in a nested module directory, because go:embed cannot embed
// files of another module and the gate ships as embedded bytes. Refresh all three together:
// change the two requirements, run "go mod tidy" in a scratch directory holding them and copy
// the three files back (docs/guides/api-compatibility.md, "The pinned checker"). Renovate moves
// the two requirements (renovate.json) and TestGatePinsItsChecker fails the branch until go.sum
// covers them.
const (
	checkerGoMod = `module apicompat.invalid/checker

go 1.26.0

require (
	github.com/joelanford/go-apidiff v0.8.4-0.20260910211158-c3e0953fa2fd
	golang.org/x/tools v0.51.0
)

require (
	dario.cat/mergo v1.0.2 // indirect
	github.com/Microsoft/go-winio v0.6.2 // indirect
	github.com/ProtonMail/go-crypto v1.2.0 // indirect
	github.com/cloudflare/circl v1.6.3 // indirect
	github.com/cyphar/filepath-securejoin v0.6.1 // indirect
	github.com/emirpasic/gods v1.18.1 // indirect
	github.com/go-git/gcfg v1.5.1-0.20230307220236-3a3c6141e376 // indirect
	github.com/go-git/go-billy/v5 v5.9.1 // indirect
	github.com/go-git/go-git/v5 v5.19.2 // indirect
	github.com/golang/groupcache v0.0.0-20241129210726-2c02b8208cf8 // indirect
	github.com/inconshreveable/mousetrap v1.1.0 // indirect
	github.com/jbenet/go-context v0.0.0-20150711004518-d14ea06fba99 // indirect
	github.com/kevinburke/ssh_config v1.2.0 // indirect
	github.com/klauspost/cpuid/v2 v2.3.0 // indirect
	github.com/pjbgf/sha1cd v0.6.0 // indirect
	github.com/sergi/go-diff v1.3.2-0.20230802210424-5b0b94c5c0d3 // indirect
	github.com/skeema/knownhosts v1.3.1 // indirect
	github.com/spf13/cobra v1.10.2 // indirect
	github.com/spf13/pflag v1.0.9 // indirect
	github.com/xanzy/ssh-agent v0.3.3 // indirect
	golang.org/x/crypto v0.57.0 // indirect
	golang.org/x/exp v0.0.0-20260410095643-746e56fc9e2f // indirect
	golang.org/x/mod v0.41.0 // indirect
	golang.org/x/net v0.59.0 // indirect
	golang.org/x/sync v0.23.0 // indirect
	golang.org/x/sys v0.48.0 // indirect
	gopkg.in/warnings.v0 v0.1.2 // indirect
)
`
	checkerTools = `//go:build tools

package tools

import (
	_ "github.com/joelanford/go-apidiff"
	_ "golang.org/x/tools/go/packages"
)
`
	checkerGoSum = `dario.cat/mergo v1.0.2 h1:85+piFYR1tMbRrLcDwR18y4UKJ3aH1Tbzi24VRW1TK8=
dario.cat/mergo v1.0.2/go.mod h1:E/hbnu0NxMFBjpMIE34DRGLWqDy0g5FuKDhCb31ngxA=
github.com/Microsoft/go-winio v0.5.2/go.mod h1:WpS1mjBmmwHBEWmogvA2mj8546UReBk4v8QkMxJ6pZY=
github.com/Microsoft/go-winio v0.6.2 h1:F2VQgta7ecxGYO8k3ZZz3RS8fVIXVxONVUPlNERoyfY=
github.com/Microsoft/go-winio v0.6.2/go.mod h1:yd8OoFMLzJbo9gZq8j5qaps8bJ9aShtEA8Ipt1oGCvU=
github.com/ProtonMail/go-crypto v1.2.0 h1:+PhXXn4SPGd+qk76TlEePBfOfivE0zkWFenhGhFLzWs=
github.com/ProtonMail/go-crypto v1.2.0/go.mod h1:9whxjD8Rbs29b4XWbB8irEcE8KHMqaR2e7GWU1R+/PE=
github.com/anmitsu/go-shlex v0.0.0-20200514113438-38f4b401e2be h1:9AeTilPcZAjCFIImctFaOjnTIavg87rW78vTPkQqLI8=
github.com/anmitsu/go-shlex v0.0.0-20200514113438-38f4b401e2be/go.mod h1:ySMOLuWl6zY27l47sB3qLNK6tF2fkHG55UZxx8oIVo4=
github.com/armon/go-socks5 v0.0.0-20160902184237-e75332964ef5 h1:0CwZNZbxp69SHPdPJAN/hZIm0C4OItdklCFmMRWYpio=
github.com/armon/go-socks5 v0.0.0-20160902184237-e75332964ef5/go.mod h1:wHh0iHkYZB8zMSxRWpUBQtwG5a7fFgvEO+odwuTv2gs=
github.com/cloudflare/circl v1.6.3 h1:9GPOhQGF9MCYUeXyMYlqTR6a5gTrgR/fBLXvUgtVcg8=
github.com/cloudflare/circl v1.6.3/go.mod h1:2eXP6Qfat4O/Yhh8BznvKnJ+uzEoTQ6jVKJRn81BiS4=
github.com/cpuguy83/go-md2man/v2 v2.0.6/go.mod h1:oOW0eioCTA6cOiMLiUPZOpcVxMig6NIQQ7OS05n1F4g=
github.com/cyphar/filepath-securejoin v0.6.1 h1:5CeZ1jPXEiYt3+Z6zqprSAgSWiggmpVyciv8syjIpVE=
github.com/cyphar/filepath-securejoin v0.6.1/go.mod h1:A8hd4EnAeyujCJRrICiOWqjS1AX0a9kM5XL+NwKoYSc=
github.com/davecgh/go-spew v1.1.0/go.mod h1:J7Y8YcW2NihsgmVo/mv3lAwl/skON4iLHjSsI+c5H38=
github.com/davecgh/go-spew v1.1.1 h1:vj9j/u1bqnvCEfJOwUhtlOARqs3+rkHYY13jYWTU97c=
github.com/davecgh/go-spew v1.1.1/go.mod h1:J7Y8YcW2NihsgmVo/mv3lAwl/skON4iLHjSsI+c5H38=
github.com/elazarl/goproxy v1.7.2 h1:Y2o6urb7Eule09PjlhQRGNsqRfPmYI3KKQLFpCAV3+o=
github.com/elazarl/goproxy v1.7.2/go.mod h1:82vkLNir0ALaW14Rc399OTTjyNREgmdL2cVoIbS6XaE=
github.com/emirpasic/gods v1.18.1 h1:FXtiHYKDGKCW2KzwZKx0iC0PQmdlorYgdFG9jPXJ1Bc=
github.com/emirpasic/gods v1.18.1/go.mod h1:8tpGGwCnJ5H4r6BWwaV6OrWmMoPhUl5jm/FMNAnJvWQ=
github.com/gliderlabs/ssh v0.3.8 h1:a4YXD1V7xMF9g5nTkdfnja3Sxy1PVDCj1Zg4Wb8vY6c=
github.com/gliderlabs/ssh v0.3.8/go.mod h1:xYoytBv1sV0aL3CavoDuJIQNURXkkfPA/wxQ1pL1fAU=
github.com/go-git/gcfg v1.5.1-0.20230307220236-3a3c6141e376 h1:+zs/tPmkDkHx3U66DAb0lQFJrpS6731Oaa12ikc+DiI=
github.com/go-git/gcfg v1.5.1-0.20230307220236-3a3c6141e376/go.mod h1:an3vInlBmSxCcxctByoQdvwPiA7DTK7jaaFDBTtu0ic=
github.com/go-git/go-billy/v5 v5.9.1 h1:8U73XiOTfINdItHVa6z4Gv7ToObcZ6grkqQbLryLCdA=
github.com/go-git/go-billy/v5 v5.9.1/go.mod h1:ExsU+jcGwXTBOnyilvAnEM1wug1IxHr4yP2ZXsNRtV0=
github.com/go-git/go-git-fixtures/v4 v4.3.2-0.20231010084843-55a94097c399 h1:eMje31YglSBqCdIqdhKBW8lokaMrL3uTkpGYlE2OOT4=
github.com/go-git/go-git-fixtures/v4 v4.3.2-0.20231010084843-55a94097c399/go.mod h1:1OCfN199q1Jm3HZlxleg+Dw/mwps2Wbk9frAWm+4FII=
github.com/go-git/go-git/v5 v5.19.2 h1:wkfn7vOlUBu8ivAWKBWisTiwJK4jYHzTF8Ndv1LyGqY=
github.com/go-git/go-git/v5 v5.19.2/go.mod h1:QqCBE1EFN5ddFmrliLQ3/ntRCUjZU3EJuwuB/jWEHjk=
github.com/golang/groupcache v0.0.0-20241129210726-2c02b8208cf8 h1:f+oWsMOmNPc8JmEHVZIycC7hBoQxHH9pNKQORJNozsQ=
github.com/golang/groupcache v0.0.0-20241129210726-2c02b8208cf8/go.mod h1:wcDNUvekVysuuOpQKo3191zZyTpiI6se1N1ULghS0sw=
github.com/google/go-cmp v0.7.0 h1:wk8382ETsv4JYUZwIsn6YpYiWiBsYLSJiTsyBybVuN8=
github.com/google/go-cmp v0.7.0/go.mod h1:pXiqmnSA92OHEEa9HXL2W4E7lf9JzCmGVUdgjX3N/iU=
github.com/inconshreveable/mousetrap v1.1.0 h1:wN+x4NVGpMsO7ErUn/mUI3vEoE6Jt13X2s0bqwp9tc8=
github.com/inconshreveable/mousetrap v1.1.0/go.mod h1:vpF70FUmC8bwa3OWnCshd2FqLfsEA9PFc4w1p2J65bw=
github.com/jbenet/go-context v0.0.0-20150711004518-d14ea06fba99 h1:BQSFePA1RWJOlocH6Fxy8MmwDt+yVQYULKfN0RoTN8A=
github.com/jbenet/go-context v0.0.0-20150711004518-d14ea06fba99/go.mod h1:1lJo3i6rXxKeerYnT8Nvf0QmHCRC1n8sfWVwXF2Frvo=
github.com/joelanford/go-apidiff v0.8.4-0.20260910211158-c3e0953fa2fd h1:wlybhuDMr6L5NFy1uR8iV0wLmGNuIDQg0b0NkUj1IPo=
github.com/joelanford/go-apidiff v0.8.4-0.20260910211158-c3e0953fa2fd/go.mod h1:wF5i6giMX2ISm5UBkIo/RerIPqluJ4BqfwAwQy6YjY4=
github.com/kevinburke/ssh_config v1.2.0 h1:x584FjTGwHzMwvHx18PXxbBVzfnxogHaAReU4gf13a4=
github.com/kevinburke/ssh_config v1.2.0/go.mod h1:CT57kijsi8u/K/BOFA39wgDQJ9CxiF4nAY/ojJ6r6mM=
github.com/klauspost/cpuid/v2 v2.3.0 h1:S4CRMLnYUhGeDFDqkGriYKdfoFlDnMtqTiI/sFzhA9Y=
github.com/klauspost/cpuid/v2 v2.3.0/go.mod h1:hqwkgyIinND0mEev00jJYCxPNVRVXFQeu1XKlok6oO0=
github.com/kr/pretty v0.1.0/go.mod h1:dAy3ld7l9f0ibDNOQOHHMYYIIbhfbHSm3C4ZsoJORNo=
github.com/kr/pretty v0.3.1 h1:flRD4NNwYAUpkphVc1HcthR4KEIFJ65n8Mw5qdRn3LE=
github.com/kr/pretty v0.3.1/go.mod h1:hoEshYVHaxMs3cyo3Yncou5ZscifuDolrwPKZanG3xk=
github.com/kr/pty v1.1.1/go.mod h1:pFQYn66WHrOpPYNljwOMqo10TkYh1fy3cYio2l3bCsQ=
github.com/kr/text v0.1.0/go.mod h1:4Jbv+DJW3UT/LiOwJeYQe1efqtUx/iVham/4vfdArNI=
github.com/kr/text v0.2.0 h1:5Nx0Ya0ZqY2ygV366QzturHI13Jq95ApcVaJBhpS+AY=
github.com/kr/text v0.2.0/go.mod h1:eLer722TekiGuMkidMxC/pM04lWEeraHUUmBw8l2grE=
github.com/onsi/gomega v1.34.1 h1:EUMJIKUjM8sKjYbtxQI9A4z2o+rruxnzNvpknOXie6k=
github.com/onsi/gomega v1.34.1/go.mod h1:kU1QgUvBDLXBJq618Xvm2LUX6rSAfRaFRTcdOeDLwwY=
github.com/pjbgf/sha1cd v0.6.0 h1:3WJ8Wz8gvDz29quX1OcEmkAlUg9diU4GxJHqs0/XiwU=
github.com/pjbgf/sha1cd v0.6.0/go.mod h1:lhpGlyHLpQZoxMv8HcgXvZEhcGs0PG/vsZnEJ7H0iCM=
github.com/pkg/errors v0.9.1 h1:FEBLx1zS214owpjy7qsBeixbURkuhQAwrK5UwLGTwt4=
github.com/pkg/errors v0.9.1/go.mod h1:bwawxfHBFNV+L2hUp1rHADufV3IMtnDRdf1r5NINEl0=
github.com/pmezard/go-difflib v1.0.0 h1:4DBwDE0NGyQoBHbLQYPwSUPoCMWR5BEzIk/f1lZbAQM=
github.com/pmezard/go-difflib v1.0.0/go.mod h1:iKH77koFhYxTK1pcRnkKkqfTogsbg7gZNVY4sRDYZ/4=
github.com/rogpeppe/go-internal v1.14.1 h1:UQB4HGPB6osV0SQTLymcB4TgvyWu6ZyliaW0tI/otEQ=
github.com/rogpeppe/go-internal v1.14.1/go.mod h1:MaRKkUm5W0goXpeCfT7UZI6fk/L7L7so1lCWt35ZSgc=
github.com/russross/blackfriday/v2 v2.1.0/go.mod h1:+Rmxgy9KzJVeS9/2gXHxylqXiyQDYRxCVz55jmeOWTM=
github.com/sergi/go-diff v1.3.2-0.20230802210424-5b0b94c5c0d3 h1:n661drycOFuPLCN3Uc8sB6B/s6Z4t2xvBgU1htSHuq8=
github.com/sergi/go-diff v1.3.2-0.20230802210424-5b0b94c5c0d3/go.mod h1:A0bzQcvG0E7Rwjx0REVgAGH58e96+X0MeOfepqsbeW4=
github.com/sirupsen/logrus v1.7.0/go.mod h1:yWOB1SBYBC5VeMP7gHvWumXLIWorT60ONWic61uBYv0=
github.com/skeema/knownhosts v1.3.1 h1:X2osQ+RAjK76shCbvhHHHVl3ZlgDm8apHEHFqRjnBY8=
github.com/skeema/knownhosts v1.3.1/go.mod h1:r7KTdC8l4uxWRyK2TpQZ/1o5HaSzh06ePQNxPwTcfiY=
github.com/spf13/cobra v1.10.2 h1:DMTTonx5m65Ic0GOoRY2c16WCbHxOOw6xxezuLaBpcU=
github.com/spf13/cobra v1.10.2/go.mod h1:7C1pvHqHw5A4vrJfjNwvOdzYu0Gml16OCs2GRiTUUS4=
github.com/spf13/pflag v1.0.9 h1:9exaQaMOCwffKiiiYk6/BndUBv+iRViNW+4lEMi0PvY=
github.com/spf13/pflag v1.0.9/go.mod h1:McXfInJRrz4CZXVZOBLb0bTZqETkiAhM9Iw0y3An2Bg=
github.com/stretchr/objx v0.1.0/go.mod h1:HFkY916IF+rwdDfMAkV7OtwuqBVzrE8GR6GFx+wExME=
github.com/stretchr/testify v1.2.2/go.mod h1:a8OnRcib4nhh0OaRAV+Yts87kKdq0PP7pXfy6kDkUVs=
github.com/stretchr/testify v1.4.0/go.mod h1:j7eGeouHqKxXV5pUuKE4zz7dFj8WfuZ+81PSLYec5m4=
github.com/stretchr/testify v1.11.1 h1:7s2iGBzp5EwR7/aIZr8ao5+dra3wiQyKjjFuvgVKu7U=
github.com/stretchr/testify v1.11.1/go.mod h1:wZwfW3scLgRK+23gO65QZefKpKQRnfz6sD981Nm4B6U=
github.com/xanzy/ssh-agent v0.3.3 h1:+/15pJfg/RsTxqYcX6fHqOXZwwMP+2VyYWJeWM2qQFM=
github.com/xanzy/ssh-agent v0.3.3/go.mod h1:6dzNDKs0J9rVPHPhaGCukekBHKqfl+L3KghI1Bc68Uw=
go.yaml.in/yaml/v3 v3.0.4/go.mod h1:DhzuOOF2ATzADvBadXxruRBLzYTpT36CKvDb3+aBEFg=
golang.org/x/crypto v0.0.0-20220622213112-05595931fe9d/go.mod h1:IxCIyHEi3zRg3s0A5j5BB6A9Jmi73HwBIUl50j+osU4=
golang.org/x/crypto v0.57.0 h1:3ZVCjf8Ggz7zneR/EHRVx68Ctf+2pmIMP2UFhh9cC6M=
golang.org/x/crypto v0.57.0/go.mod h1:Fdz0i5U6CoizGwLda9DttjSk6qlZo25zYNtR+ycvuZA=
golang.org/x/exp v0.0.0-20260410095643-746e56fc9e2f h1:W3F4c+6OLc6H2lb//N1q4WpJkhzJCK5J6kUi1NTVXfM=
golang.org/x/exp v0.0.0-20260410095643-746e56fc9e2f/go.mod h1:J1xhfL/vlindoeF/aINzNzt2Bket5bjo9sdOYzOsU80=
golang.org/x/mod v0.41.0 h1:qJmnOUb4YB+FsEuM3HcWucdZASCPGhsX6uljO6pog0c=
golang.org/x/mod v0.41.0/go.mod h1:Ek9pY8RKWXwsWvd3rQiHYtMqkjSUV+s1Rj7j4H5Ur6o=
golang.org/x/net v0.0.0-20211112202133-69e39bad7dc2/go.mod h1:9nx3DQGgdP8bBQD5qxJ1jj9UTztislL4KSBs9R2vV5Y=
golang.org/x/net v0.59.0 h1:5zfYln+w5XCxwrnMMJPufRgNoXEaGxl0wo5GqPXyues=
golang.org/x/net v0.59.0/go.mod h1:2DA/G1UfVbCpQPeWTmMPGY7Cs2PkBkwu743bVX5PIVg=
golang.org/x/sync v0.23.0 h1:KameEIfc1IkluZyXWLn39Wd4tURc6GbCiISGiZm2bQk=
golang.org/x/sync v0.23.0/go.mod h1:sUUOizhqBxiL6pEWpqNLUiaJn1ShEbZ6BBqskPbjZm0=
golang.org/x/sys v0.0.0-20191026070338-33540a1f6037/go.mod h1:h1NjWce9XRLGQEsW7wpKNCjG9DtNlClVuFLEZdDNbEs=
golang.org/x/sys v0.0.0-20201119102817-f84b799fce68/go.mod h1:h1NjWce9XRLGQEsW7wpKNCjG9DtNlClVuFLEZdDNbEs=
golang.org/x/sys v0.0.0-20210124154548-22da62e12c0c/go.mod h1:h1NjWce9XRLGQEsW7wpKNCjG9DtNlClVuFLEZdDNbEs=
golang.org/x/sys v0.0.0-20210423082822-04245dca01da/go.mod h1:h1NjWce9XRLGQEsW7wpKNCjG9DtNlClVuFLEZdDNbEs=
golang.org/x/sys v0.0.0-20210615035016-665e8c7367d1/go.mod h1:oPkhp1MJrh7nUepCBck5+mAzfO9JrbApNNgaTdGDITg=
golang.org/x/sys v0.0.0-20220715151400-c0bba94af5f8/go.mod h1:oPkhp1MJrh7nUepCBck5+mAzfO9JrbApNNgaTdGDITg=
golang.org/x/sys v0.48.0 h1:bbX/i/6MgT9BVLM9RT1thmxL04yeTAhbEz4SyadbXoo=
golang.org/x/sys v0.48.0/go.mod h1:hNLxWAXmnKAxqDtdwIYC4bM9oQPEecfsnNMuSxOs3og=
golang.org/x/term v0.0.0-20201126162022-7de9c90e9dd1/go.mod h1:bj7SfCRtBDWHUb9snDiAeCFNEtKQo2Wmx5Cou7ajbmo=
golang.org/x/term v0.46.0 h1:3+OXuTbaKDgwk8jTi3aSLHRlmWqHEUDUtxnbFigO4YE=
golang.org/x/term v0.46.0/go.mod h1:+K02xbkittuwc0Am4abfA3Fc+XRGXkvBXNO88NCXPoc=
golang.org/x/text v0.3.6/go.mod h1:5Zoc/QRtKVWzQhOtBMvqHzDpF6irO9z98xDceosuGiQ=
golang.org/x/text v0.42.0 h1:JbOZXgfeCPU9gacVtYliJqOhD+zhrEqK4LfdpmlUZqI=
golang.org/x/text v0.42.0/go.mod h1:ojzP1Z+2QtioaF8DTtO8K5q7JWVVYwZKenzujK0Zd0E=
golang.org/x/tools v0.0.0-20180917221912-90fa682c2a6e/go.mod h1:n7NCudcB/nEzxVGmLbDWY5pfWTLqBcC2KZ6jyYvM4mQ=
golang.org/x/tools v0.51.0 h1:k4Xc/1Om9jwkBJBo4NVLMSARBoWtK10mx+W5BnXCeAI=
golang.org/x/tools v0.51.0/go.mod h1:9eEncMayCV6zRMGhR5eZEC2iBx98qWcF1HZ9Z7wJOoA=
golang.org/x/tools/go/expect v0.1.1-deprecated h1:jpBZDwmgPhXsKZC6WhL20P4b/wmnpsEAGHaNy0n/rJM=
golang.org/x/tools/go/expect v0.1.1-deprecated/go.mod h1:eihoPOH+FgIqa3FpoTwguz/bVUSGBlGQU67vpBeOrBY=
golang.org/x/tools/go/packages/packagestest v0.1.1-deprecated h1:1h2MnaIAIXISqTFKdENegdpAgUXz6NrPEsbIeWaBRvM=
golang.org/x/tools/go/packages/packagestest v0.1.1-deprecated/go.mod h1:RVAQXBGNv1ib0J382/DPCRS/BPnsGebyM1Gj5VSDpG8=
gopkg.in/check.v1 v0.0.0-20161208181325-20d25e280405/go.mod h1:Co6ibVJAznAaIkqp8huTwlJQCZ016jof/cbN4VW5Yz0=
gopkg.in/check.v1 v1.0.0-20190902080502-41f04d3bba15/go.mod h1:Co6ibVJAznAaIkqp8huTwlJQCZ016jof/cbN4VW5Yz0=
gopkg.in/check.v1 v1.0.0-20201130134442-10cb98267c6c h1:Hei/4ADfdWqJk1ZMxUNpqntNwaWcugrBjAiHlqqRiVk=
gopkg.in/check.v1 v1.0.0-20201130134442-10cb98267c6c/go.mod h1:JHkPIbrfpd72SG/EVd6muEfDQjcINNoR0C8j2r3qZ4Q=
gopkg.in/warnings.v0 v0.1.2 h1:wFXVbFY8DY5/xOe1ECiWdKCzZlxgshcYVNkBHstARME=
gopkg.in/warnings.v0 v0.1.2/go.mod h1:jksf8JmL6Qr/oQM2OXTHunEvvTAsrWBLb6OOjuVWRNI=
gopkg.in/yaml.v2 v2.2.2/go.mod h1:hI93XBmqTisBFMUTm0b8Fm+jr3Dg1NNxqwp+5A1VGuI=
gopkg.in/yaml.v2 v2.4.0/go.mod h1:RDklbk79AGWmwhnvt/jBztapEOGDOx6ZbXqjP6csGnQ=
gopkg.in/yaml.v3 v3.0.1 h1:fxVm/GzAzEWqLHuvctI91KS9hhNmmWOoWu0XTYJS7CA=
gopkg.in/yaml.v3 v3.0.1/go.mod h1:K4uyk7z7BCEPqu6E+C64Yfv1cQ7kz7rIZviUmN+EgEM=
`
)

// releaseTagPattern matches a root release tag. A nested module's tags carry the module's
// directory as a prefix (Go modules reference, "Mapping versions to commits"), and a
// pre-release or build suffix does not publish a release, so neither matches.
var releaseTagPattern = regexp.MustCompile(`^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$`)

// The canary module verifyChecker compares: its second commit removes the exported function
// its first declares.
const (
	canaryGoMod  = "module apicompat.invalid/canary\n\ngo 1.21\n"
	canaryBefore = "package canary\n\n// Removed is deleted by the second commit.\nfunc Removed() {}\n"
	canaryAfter  = "package canary\n"
)

var errOutputLimit = errors.New("output exceeds its bound")

// gitRepositoryVariables bind git to a repository other than the one its working directory
// names. A gate run from a git hook inherits them, and the canary's git init and commits would
// then write into that repository. internal/util/command_environment.go strips the same set
// from Praetor's own child processes; this standalone program cannot import it.
var gitRepositoryVariables = [...]string{
	"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_COMMON_DIR", "GIT_CONFIG", "GIT_DIR", "GIT_GRAFT_FILE",
	"GIT_IMPLICIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_NO_REPLACE_OBJECTS", "GIT_OBJECT_DIRECTORY",
	"GIT_PREFIX", "GIT_REPLACE_REF_BASE", "GIT_SHALLOW_FILE", "GIT_WORK_TREE",
}

func main() {
	for _, name := range gitRepositoryVariables {
		if err := os.Unsetenv(name); err != nil {
			fmt.Fprintf(os.Stderr, "error: the API compatibility comparison did not run: unset %s: %v\n", name, err)
			os.Exit(exitNotRun)
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), runTimeout)
	status := run(ctx, os.Args[1:], os.Stdout, os.Stderr)
	cancel()
	os.Exit(status)
}

// run executes the gate and returns its exit status. Every error is an execution failure, and
// so is a report the gate could not write.
func run(ctx context.Context, args []string, out, errOut io.Writer) int {
	stdout, stderr := &printer{w: out}, &printer{w: errOut}
	status, err := runGate(ctx, args, stdout, stderr)
	if err != nil {
		stderr.annotate("error", "the API compatibility comparison did not run: "+err.Error())
		status = exitNotRun
	}
	if stdout.failed != nil || stderr.failed != nil {
		return exitNotRun
	}
	return status
}

func runGate(ctx context.Context, args []string, stdout, stderr *printer) (int, error) {
	opts, err := parseOptions(args, stderr.w)
	if err != nil {
		return exitNotRun, err
	}
	if opts.exceptions, err = parseExceptions(os.Getenv(exceptionsEnv)); err != nil {
		return exitNotRun, err
	}
	return gate(ctx, opts, stdout, stderr)
}

// printer writes the gate's report and keeps the first write error.
type printer struct {
	w      io.Writer
	failed error
}

func (p *printer) printf(format string, args ...any) {
	if p.failed == nil {
		_, p.failed = fmt.Fprintf(p.w, format, args...)
	}
}

// annotate prints one message as a GitHub Actions workflow command, which renders as an
// annotation, when GITHUB_ACTIONS is "true", and as a plain "level: message" line otherwise.
func (p *printer) annotate(level, message string) {
	if os.Getenv("GITHUB_ACTIONS") == "true" {
		p.printf("::%s::%s\n", level, strings.NewReplacer("%", "%25", "\r", "%0D", "\n", "%0A").Replace(message))
		return
	}
	p.printf("%s: %s\n", level, message)
}

// options are the gate's flags.
type options struct {
	repo, base, policy, checker string
	exceptions                  []exception
}

// exception is one live-or-expired entry of the manifest's api-compatibility exceptions: the
// go.mod of a module whose build failure is excused until the end of the day expires.
type exception struct {
	Path    string `json:"path"`
	Reason  string `json:"reason"`
	Expires string `json:"expires"`
}

// parseExceptions reads the JSON list in exceptionsEnv. An empty value is no exception; a value
// that is not a bounded list of well-formed entries fails the run, so a damaged workflow never
// reads as "no exceptions" by accident.
func parseExceptions(raw string) ([]exception, error) {
	if strings.TrimSpace(raw) == "" {
		return nil, nil
	}
	decoder := json.NewDecoder(strings.NewReader(raw))
	decoder.DisallowUnknownFields()
	var entries []exception
	if err := decoder.Decode(&entries); err != nil {
		return nil, fmt.Errorf("%s is not a JSON list of {path, reason, expires}: %w", exceptionsEnv, err)
	}
	if len(entries) > maxExceptions {
		return nil, fmt.Errorf("%s lists %d exceptions, want at most %d", exceptionsEnv, len(entries), maxExceptions)
	}
	for index := 0; index < len(entries) && index < maxExceptions; index++ {
		entry := entries[index]
		if path.Base(entry.Path) != "go.mod" || path.Clean(entry.Path) != entry.Path || strings.TrimSpace(entry.Reason) == "" {
			return nil, fmt.Errorf("%s entry %d must name a module's go.mod and give a reason", exceptionsEnv, index)
		}
		if _, err := time.Parse(exceptionsDate, entry.Expires); err != nil {
			return nil, fmt.Errorf("%s entry %d: expires %q is not a YYYY-MM-DD date", exceptionsEnv, index, entry.Expires)
		}
	}
	return entries, nil
}

// exceptionFor returns what entries say about the module in dir ("." for the root module) on
// today: the last entry that still holds, or else the first expired one. Both are nil without an
// entry for the module.
func exceptionFor(entries []exception, dir string, today time.Time) (live, expired *exception) {
	for index := 0; index < len(entries) && index < maxExceptions; index++ {
		if path.Dir(entries[index].Path) != dir {
			continue
		}
		until, err := time.Parse(exceptionsDate, entries[index].Expires)
		switch {
		case err == nil && !until.Before(today):
			live = &entries[index]
		case expired == nil:
			expired = &entries[index]
		}
	}
	return live, expired
}

func parseOptions(args []string, stderr io.Writer) (options, error) {
	flags := flag.NewFlagSet("gate", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var opts options
	flags.StringVar(&opts.repo, "repo", ".", "any directory inside the repository to compare")
	flags.StringVar(&opts.base, "base", "", "base revision; empty selects the newest root release tag merged into HEAD")
	flags.StringVar(&opts.policy, "policy", "auto", "compatibility policy: auto, warn or reject")
	flags.StringVar(&opts.checker, "checker", "", "go-apidiff binary; empty builds "+checkerPackage+" from the pinned build module")
	if err := flags.Parse(args); err != nil {
		return options{}, err
	}
	if flags.NArg() != 0 {
		return options{}, fmt.Errorf("unexpected arguments %q", flags.Args())
	}
	if !slices.Contains([]string{"auto", "warn", "reject"}, opts.policy) {
		return options{}, fmt.Errorf("-policy must be auto, warn or reject, got %q", opts.policy)
	}
	if strings.HasPrefix(opts.base, "-") || len(opts.base) > maxRevisionBytes {
		return options{}, fmt.Errorf("-base %q is not a revision name", opts.base)
	}
	return opts, nil
}

// gate resolves the revisions and the policy, discovers the modules at both revisions and
// compares them, returning the exit status for the verdict.
func gate(ctx context.Context, opts options, stdout, stderr *printer) (int, error) {
	repo, err := resolveRepository(ctx, opts)
	if err != nil {
		return exitNotRun, err
	}
	repo.exceptions = opts.exceptions
	year, month, day := time.Now().UTC().Date()
	repo.today = time.Date(year, month, day, 0, 0, 0, 0, time.UTC)
	if repo.base == "" {
		stdout.printf("No root release tag vMAJOR.MINOR.PATCH is merged into HEAD %s and -base is unset: "+
			"no published API to compare.\n", short(repo.head))
		return exitCompatible, nil
	}
	if repo.base == repo.head {
		stdout.printf("Base %s is HEAD %s: nothing to compare.\n", repo.baseName, short(repo.head))
		return exitCompatible, nil
	}
	plan, err := planModules(ctx, repo)
	if err != nil {
		return exitNotRun, err
	}
	results, err := compareModules(ctx, repo, plan, opts.checker, stdout, stderr)
	if err != nil {
		return exitNotRun, err
	}
	return verdict(repo, plan, results, stdout, stderr), nil
}

// repository is what one gate run compares: the root, the two commits and the policy.
type repository struct {
	root     string
	head     string
	base     string
	baseName string
	policy   policy
	// exceptions are the manifest's module exceptions and today the UTC day they are judged on.
	exceptions []exception
	today      time.Time
}

// policy is the compatibility policy: whether an incompatible change fails the gate, and why.
type policy struct {
	reject bool
	reason string
}

// releaseTag is the newest root release tag merged into HEAD.
type releaseTag struct {
	name    string
	version [3]int
	found   bool
}

func resolveRepository(ctx context.Context, opts options) (repository, error) {
	top, err := gitCapture(ctx, opts.repo, maxRevisionBytes, "rev-parse", "--show-toplevel")
	if err != nil {
		return repository{}, err
	}
	repo := repository{root: filepath.Clean(filepath.FromSlash(strings.TrimSpace(string(top))))}
	if repo.head, err = commitOf(ctx, repo.root, "HEAD"); err != nil {
		return repository{}, err
	}
	newest, err := newestReleaseTag(ctx, repo.root, repo.head)
	if err != nil {
		return repository{}, err
	}
	repo.policy = resolvePolicy(opts.policy, newest)
	switch {
	case opts.base != "":
		repo.baseName = opts.base
		repo.base, err = commitOf(ctx, repo.root, opts.base)
	case newest.found:
		repo.baseName = newest.name
		repo.base, err = commitOf(ctx, repo.root, "refs/tags/"+newest.name)
	}
	return repo, err
}

// commitOf resolves revision to the full name of the commit it names.
func commitOf(ctx context.Context, root, revision string) (string, error) {
	commit, err := gitOutput(ctx, root, "rev-parse", "--verify", "--quiet", revision+"^{commit}")
	if err != nil {
		return "", fmt.Errorf("revision %q does not name a commit: %w", revision, err)
	}
	return commit, nil
}

// newestReleaseTag returns the highest root release tag merged into head.
func newestReleaseTag(ctx context.Context, root, head string) (releaseTag, error) {
	tags, err := gitRecords(ctx, root, '\n', maxTags, func(string) bool { return true },
		"tag", "--merged", head, "--list", "v*")
	if err != nil {
		return releaseTag{}, err
	}
	var newest releaseTag
	for index := 0; index < len(tags) && index < maxTags; index++ {
		candidate, ok, err := parseReleaseTag(tags[index])
		if err != nil {
			return releaseTag{}, err
		}
		if ok && (!newest.found || slices.Compare(candidate.version[:], newest.version[:]) > 0) {
			newest = candidate
		}
	}
	return newest, nil
}

// parseReleaseTag reads name as a root release tag; ok is false for any other tag.
func parseReleaseTag(name string) (releaseTag, bool, error) {
	match := releaseTagPattern.FindStringSubmatch(name)
	if match == nil {
		return releaseTag{}, false, nil
	}
	tag := releaseTag{name: name, found: true}
	for index := range tag.version {
		value, err := strconv.Atoi(match[index+1])
		if err != nil {
			return releaseTag{}, false, fmt.Errorf("release tag %s: %w", name, err)
		}
		tag.version[index] = value
	}
	return tag, true, nil
}

// resolvePolicy turns the -policy flag into the compatibility policy. auto follows the
// release line: a v1 or later root release promises a stable API, a v0 line or no release
// does not.
func resolvePolicy(flagValue string, newest releaseTag) policy {
	switch {
	case flagValue == "warn":
		return policy{reason: "-policy=warn"}
	case flagValue == "reject":
		return policy{reject: true, reason: "-policy=reject"}
	case !newest.found:
		return policy{reason: "auto: no root release tag, so the API is pre-v1"}
	case newest.version[0] >= 1:
		return policy{reject: true, reason: "auto: newest root release tag " + newest.name + " is v1 or later"}
	default:
		return policy{reason: "auto: newest root release tag " + newest.name + " is pre-v1"}
	}
}

// module is one go.mod a revision tracks. from is the base revision's module path in the same
// directory, set only for a new major version.
type module struct {
	dir  string
	path string
	from string
}

// modulePlan sorts the modules of both revisions by directory: compared at both, a new major
// version at HEAD of the base's module, added at HEAD only, removed from HEAD, and not public
// because the module path has an internal element.
type modulePlan struct {
	compared  []module
	newMajor  []module
	added     []module
	removed   []module
	notPublic []module
}

func (p modulePlan) empty() bool {
	return len(p.compared)+len(p.newMajor)+len(p.added)+len(p.removed)+len(p.notPublic) == 0
}

func planModules(ctx context.Context, repo repository) (modulePlan, error) {
	base, err := discoverModules(ctx, repo.root, repo.base)
	if err != nil {
		return modulePlan{}, err
	}
	head, err := discoverModules(ctx, repo.root, repo.head)
	if err != nil {
		return modulePlan{}, err
	}
	var plan modulePlan
	base, plan.notPublic = splitPublic(base, nil)
	head, plan.notPublic = splitPublic(head, plan.notPublic)
	for index := 0; index < len(head) && index < maxModules; index++ {
		plan.pair(base, head[index])
	}
	for index := 0; index < len(base) && index < maxModules; index++ {
		if !containsDir(head, base[index].dir) {
			plan.removed = append(plan.removed, base[index])
		}
	}
	return plan, nil
}

// pair sorts one HEAD module against the base module in its directory: none there is an added
// module, a later major version of it is a new major version, and anything else is compared.
func (p *modulePlan) pair(base []module, target module) {
	at := slices.IndexFunc(base, func(m module) bool { return m.dir == target.dir })
	switch {
	case at < 0:
		p.added = append(p.added, target)
	case isNewMajor(base[at].path, target.path):
		target.from = base[at].path
		p.newMajor = append(p.newMajor, target)
	default:
		p.compared = append(p.compared, target)
	}
}

// isNewMajor reports whether headPath is a later major version of basePath: the same prefix
// with a higher major version suffix (Go modules reference, "Major version suffixes"). Importers
// of basePath keep building against its own releases, so nothing it exported is removed for
// them.
func isNewMajor(basePath, headPath string) bool {
	basePrefix, baseMajor := splitMajor(basePath)
	headPrefix, headMajor := splitMajor(headPath)
	return basePrefix == headPrefix && headMajor > baseMajor
}

// splitMajor splits a module path into the prefix before its major version suffix and the
// major version that suffix names: /vN with N of 2 or more, or .vN for a gopkg.in path. A path
// without a valid suffix is its own prefix and serves major version 1, which covers v0 too.
func splitMajor(modulePath string) (string, int) {
	separator, least := "/v", 2
	if strings.HasPrefix(modulePath, "gopkg.in/") {
		separator, least = ".v", 0
	}
	index := strings.LastIndex(modulePath, separator)
	if index <= 0 {
		return modulePath, 1
	}
	digits := modulePath[index+len(separator):]
	major, err := strconv.Atoi(digits)
	if err != nil || strconv.Itoa(major) != digits || major < least {
		return modulePath, 1
	}
	return modulePath[:index], major
}

// splitPublic returns the modules whose path has no internal element, and appends the others
// to notPublic once per directory.
func splitPublic(modules, notPublic []module) (public, skipped []module) {
	skipped = notPublic
	for index := 0; index < len(modules) && index < maxModules; index++ {
		if !slices.Contains(strings.Split(modules[index].path, "/"), "internal") {
			public = append(public, modules[index])
		} else if !containsDir(skipped, modules[index].dir) {
			skipped = append(skipped, modules[index])
		}
	}
	return public, skipped
}

func containsDir(modules []module, dir string) bool {
	return slices.ContainsFunc(modules, func(m module) bool { return m.dir == dir })
}

// discoverModules lists the modules revision tracks, in directory order.
func discoverModules(ctx context.Context, root, revision string) ([]module, error) {
	files, err := gitRecords(ctx, root, 0, maxTreeEntries, isModuleFile,
		"ls-tree", "-r", "-z", "--full-tree", "--name-only", revision)
	if err != nil {
		return nil, fmt.Errorf("list the files of %s: %w", short(revision), err)
	}
	if len(files) > maxModules {
		return nil, fmt.Errorf("%s tracks %d Go modules, more than the %d one run compares", short(revision), len(files), maxModules)
	}
	slices.Sort(files)
	modules := make([]module, 0, len(files))
	for index := 0; index < len(files) && index < maxModules; index++ {
		data, err := gitCapture(ctx, root, maxGoModBytes, "cat-file", "blob", revision+":"+files[index])
		if err != nil {
			return nil, err
		}
		modulePath, err := declaredModulePath(string(data))
		if err != nil {
			return nil, fmt.Errorf("%s:%s: %w", short(revision), files[index], err)
		}
		modules = append(modules, module{dir: path.Dir(files[index]), path: modulePath})
	}
	return modules, nil
}

// isModuleFile reports whether a tracked path is a go.mod the go command would build: one in no
// directory a ./... pattern skips.
func isModuleFile(name string) bool {
	if path.Base(name) != "go.mod" {
		return false
	}
	elements := strings.Split(path.Dir(name), "/")
	if len(elements) > maxPathElements {
		return false
	}
	return !slices.ContainsFunc(elements, skippedDirectory)
}

// skippedDirectory reports whether the go command skips a directory of this name in a ./...
// pattern (go help packages).
func skippedDirectory(name string) bool {
	if name == "." {
		return false
	}
	return name == "testdata" || name == "vendor" || strings.HasPrefix(name, ".") || strings.HasPrefix(name, "_")
}

// declaredModulePath returns the path of a go.mod's module directive: bare, double-quoted or
// back-quoted, with an optional trailing comment.
func declaredModulePath(gomod string) (string, error) {
	lines := strings.Split(gomod, "\n")
	for index := 0; index < len(lines) && index < maxGoModLines; index++ {
		value, found := moduleDirective(lines[index])
		if !found {
			continue
		}
		modulePath, err := unquotedModulePath(value)
		if err != nil {
			return "", fmt.Errorf("module directive %q: %w", strings.TrimSpace(lines[index]), err)
		}
		return modulePath, nil
	}
	return "", errors.New("go.mod declares no module directive")
}

// moduleDirective returns the argument of line when line is a module directive.
func moduleDirective(line string) (string, bool) {
	line, _, _ = strings.Cut(line, "//")
	rest, found := strings.CutPrefix(strings.TrimSpace(line), "module")
	if !found || rest == "" || !strings.ContainsRune(" \t\"`", rune(rest[0])) {
		return "", false
	}
	return strings.TrimSpace(rest), true
}

// unquotedModulePath reads a module directive's argument as one module path.
func unquotedModulePath(value string) (string, error) {
	if strings.HasPrefix(value, "\"") || strings.HasPrefix(value, "`") {
		unquoted, err := strconv.Unquote(value)
		if err != nil {
			return "", err
		}
		value = unquoted
	}
	if value == "" || strings.ContainsAny(value, " \t()") {
		return "", errors.New("names no module path")
	}
	return value, nil
}

// result is the checker's verdict on one compared module.
type result struct {
	module       module
	incompatible bool
	// notCompared, when set, says why the module was not compared: the exception that excuses
	// its build failure.
	notCompared string
}

// compareModules verifies the checker, then builds and compares each compared module in
// directory order. The first failure stops the comparison: the checker may have left another
// revision checked out.
func compareModules(
	ctx context.Context, repo repository, plan modulePlan, explicit string, stdout, stderr *printer,
) (results []result, err error) {
	if len(plan.compared) == 0 {
		return nil, nil
	}
	checker, cleanup, err := resolveChecker(ctx, explicit, stderr.w)
	if err != nil {
		return nil, err
	}
	defer func() { err = errors.Join(err, cleanup()) }()
	if err := verifyChecker(ctx, checker, stderr.w); err != nil {
		return nil, err
	}
	sites, release, err := checkerSitesFor(ctx, repo, plan, stderr.w)
	if err != nil {
		return nil, err
	}
	defer func() { err = errors.Join(err, release()) }()
	return compareEach(ctx, repo, plan, checker, sites, stdout, stderr)
}

// compareEach builds and compares each compared module in directory order, stopping at the
// first failure.
func compareEach(
	ctx context.Context, repo repository, plan modulePlan, checker string, sites checkerSites, stdout, stderr *printer,
) ([]result, error) {
	results := make([]result, 0, len(plan.compared))
	for index := 0; index < len(plan.compared) && index < maxModules; index++ {
		target := plan.compared[index]
		stdout.printf("Comparing %s (%s) from %s to %s\n", target.dir, target.path, short(repo.base), short(repo.head))
		buildErr := buildModule(ctx, repo.root, target.dir, stderr.w)
		if buildErr != nil {
			note, err := excuseBuildFailure(repo, target, buildErr, stderr)
			if err != nil {
				return nil, err
			}
			results = append(results, result{module: target, notCompared: note})
			continue
		}
		incompatible, err := compareModule(ctx, checker, sites.of(target.dir), target.dir, stdout.w, stderr.w)
		if err != nil {
			return nil, fmt.Errorf("module %s (%s): %w", target.dir, target.path, err)
		}
		results = append(results, result{module: target, incompatible: incompatible})
	}
	return results, nil
}

// buildModule builds the module's packages at HEAD in the repository at root. The checker reads
// a package it cannot type-check as empty without saying so, so a module that does not build
// would otherwise read as compatible; go list -export builds what the checker loads and writes
// nothing into the working tree.
func buildModule(ctx context.Context, root, dir string, stderr io.Writer) error {
	moduleDir := filepath.Join(root, filepath.FromSlash(dir))
	if err := runTool(ctx, moduleDir, io.Discard, stderr, "go", "list", "-export", "./..."); err != nil {
		return fmt.Errorf("its packages do not build at HEAD, so the checker cannot read them: %w", err)
	}
	return nil
}

// excuseBuildFailure returns the note that reports target as not compared when a live exception
// names its go.mod, and the failure otherwise, naming an expired exception when there is one.
// The module is never compared in place of a build the exception excuses.
func excuseBuildFailure(repo repository, target module, buildErr error, stderr *printer) (string, error) {
	live, expired := exceptionFor(repo.exceptions, target.dir, repo.today)
	if live == nil {
		failure := fmt.Errorf("module %s (%s): %w", target.dir, target.path, buildErr)
		if expired != nil {
			failure = fmt.Errorf("%w; the exception for %s expired on %s (%s), so it excuses nothing",
				failure, expired.Path, expired.Expires, expired.Reason)
		}
		return "", failure
	}
	note := fmt.Sprintf("excepted until %s: %s", live.Expires, live.Reason)
	stderr.annotate("warning", fmt.Sprintf("module %s (%s) was not compared: %s; build failure: %v",
		target.dir, target.path, note, buildErr))
	return note, nil
}

// compareModule runs the checker on the module's directory in site, after buildModule built the
// module. A vendor-free clone has no checkout, so the module's directory is created there for the
// checker to start in.
func compareModule(ctx context.Context, checker string, site checkerSite, dir string, stdout, stderr io.Writer) (bool, error) {
	checkerDir := filepath.Join(site.root, filepath.FromSlash(dir))
	if err := os.MkdirAll(checkerDir, 0o700); err != nil {
		return false, fmt.Errorf("create the checker's directory: %w", err)
	}
	return runChecker(ctx, checker, site.root, checkerDir, site.base, site.head, stdout, stderr)
}

// checkerSite is where the checker compares a module: a repository root and the two commits.
type checkerSite struct {
	root, base, head string
}

// checkerSites holds the site of the root module and the site of every nested module.
type checkerSites struct {
	root, nested checkerSite
}

func (s checkerSites) of(dir string) checkerSite {
	if dir == "." {
		return s.root
	}
	return s.nested
}

// checkerSitesFor returns where each compared module is compared: the repository itself, or,
// for a nested module while the base or HEAD tracks a root vendor entry, a vendor-free clone.
// The returned release removes the clone.
func checkerSitesFor(ctx context.Context, repo repository, plan modulePlan, stderr io.Writer) (checkerSites, func() error, error) {
	own := checkerSite{root: repo.root, base: repo.base, head: repo.head}
	sites, release := checkerSites{root: own, nested: own}, func() error { return nil }
	if !slices.ContainsFunc(plan.compared, func(m module) bool { return m.dir != "." }) {
		return sites, release, nil
	}
	vendored, err := tracksRootVendor(ctx, repo)
	if err != nil || !vendored {
		return sites, release, err
	}
	clone, release, err := vendorFreeClone(ctx, repo, stderr)
	if err != nil {
		return checkerSites{}, nil, err
	}
	sites.nested = clone
	return sites, release, nil
}

// tracksRootVendor reports whether the base or HEAD tracks an entry named vendor at the root.
func tracksRootVendor(ctx context.Context, repo repository) (bool, error) {
	revisions := [...]string{repo.base, repo.head}
	for index := 0; index < len(revisions); index++ {
		entries, err := gitRecords(ctx, repo.root, 0, len(revisions), func(string) bool { return true },
			"ls-tree", "-z", "--full-tree", revisions[index], "--", "vendor")
		if err != nil {
			return false, fmt.Errorf("list the root of %s: %w", short(revisions[index]), err)
		}
		if len(entries) != 0 {
			return true, nil
		}
	}
	return false, nil
}

// vendorFreeClone clones the repository into a temporary directory and commits the base and
// HEAD trees there without their root vendor entry, which no nested module builds from. The
// returned release removes the clone.
func vendorFreeClone(ctx context.Context, repo repository, stderr io.Writer) (checkerSite, func() error, error) {
	dir, err := os.MkdirTemp("", "apicompat-clone-")
	if err != nil {
		return checkerSite{}, nil, fmt.Errorf("create the vendor-free clone: %w", err)
	}
	release := func() error { return os.RemoveAll(dir) }
	clone, err := commitVendorFree(ctx, repo, dir, stderr)
	if err != nil {
		return checkerSite{}, nil, errors.Join(fmt.Errorf("create the vendor-free clone: %w", err), release())
	}
	return clone, release, nil
}

// commitVendorFree clones the repository into dir without a checkout and commits the vendor-free
// base and HEAD trees there. The clone's HEAD then names an empty commit, whose empty index and
// worktree the checker reads as a clean tree before it checks the two commits out itself.
func commitVendorFree(ctx context.Context, repo repository, dir string, stderr io.Writer) (checkerSite, error) {
	config := scratchGitConfig(filepath.Join(dir, "no-hooks"))
	clone := checkerSite{root: filepath.Join(dir, "repo")}
	cloneArgs := append(config, "clone", "--quiet", "--no-checkout", "--", repo.root, clone.root)
	if err := runTool(ctx, dir, io.Discard, stderr, "git", cloneArgs...); err != nil {
		return checkerSite{}, err
	}
	var err error
	if clone.base, err = vendorFreeCommit(ctx, clone.root, config, repo.base); err != nil {
		return checkerSite{}, err
	}
	if clone.head, err = vendorFreeCommit(ctx, clone.root, config, repo.head); err != nil {
		return checkerSite{}, err
	}
	return clone, emptyHead(ctx, clone.root, config)
}

// vendorFreeCommit commits the tree of revision without its root vendor entry in the clone and
// returns the commit. The clone has no checkout, so the index is the only state it changes.
func vendorFreeCommit(ctx context.Context, clone string, config []string, revision string) (string, error) {
	if _, err := gitOutput(ctx, clone, "read-tree", revision); err != nil {
		return "", err
	}
	if _, err := gitOutput(ctx, clone, "rm", "-r", "--cached", "--quiet", "--force", "--ignore-unmatch", "--", "vendor"); err != nil {
		return "", err
	}
	tree, err := gitOutput(ctx, clone, "write-tree")
	if err != nil {
		return "", err
	}
	return gitOutput(ctx, clone, append(config, "commit-tree", tree, "-m", "apicompat: "+revision+" without the root vendor entry")...)
}

// emptyHead empties the clone's index and points its HEAD at an empty commit. git reads the
// empty tree without storing it, and the checker's go-git reads only stored objects, so
// hash-object -w stores it first.
func emptyHead(ctx context.Context, clone string, config []string) error {
	if _, err := gitOutput(ctx, clone, "read-tree", "--empty"); err != nil {
		return err
	}
	tree, err := gitOutput(ctx, clone, "hash-object", "-w", "-t", "tree", "--stdin")
	if err != nil {
		return err
	}
	commit, err := gitOutput(ctx, clone, append(config, "commit-tree", tree, "-m", "apicompat: empty")...)
	if err != nil {
		return err
	}
	_, err = gitOutput(ctx, clone, append(config, "update-ref", "--no-deref", "HEAD", commit)...)
	return err
}

// scratchGitConfig is the configuration of the git commands that write the canary repository
// and the vendor-free clone: a fixed identity, no signing and a hooks directory that does not
// exist, so no configuration of the machine changes or blocks them.
func scratchGitConfig(hooks string) []string {
	return []string{
		"-c", "user.name=apicompat", "-c", "user.email=apicompat@example.invalid",
		"-c", "commit.gpgSign=false", "-c", "core.hooksPath=" + hooks,
	}
}

// resolveChecker returns the checker binary: the -checker path, or checkerPackage built from the
// pinned build module into a temporary directory the returned cleanup removes. The build runs
// outside the repository, so no go.mod, go.work or vendor directory of the repository changes
// what it builds.
func resolveChecker(ctx context.Context, explicit string, stderr io.Writer) (string, func() error, error) {
	if explicit != "" {
		return explicit, func() error { return nil }, nil
	}
	dir, err := os.MkdirTemp("", "apicompat-checker-")
	if err != nil {
		return "", nil, fmt.Errorf("create the checker directory: %w", err)
	}
	cleanup := func() error { return os.RemoveAll(dir) }
	binary, err := buildChecker(ctx, dir, stderr)
	if err != nil {
		return "", nil, errors.Join(err, cleanup())
	}
	return binary, cleanup, nil
}

// buildChecker writes the build module into dir/module and builds checkerPackage from it into
// dir. The module's go.sum pins every dependency, so the build downloads nothing it does not
// verify, and -mod=readonly refuses to change the module.
func buildChecker(ctx context.Context, dir string, stderr io.Writer) (string, error) {
	module := filepath.Join(dir, "module")
	if err := os.Mkdir(module, 0o700); err != nil {
		return "", fmt.Errorf("create the checker build module: %w", err)
	}
	files := [...]struct{ name, text string }{
		{"go.mod", checkerGoMod}, {"go.sum", checkerGoSum}, {"tools.go", checkerTools},
	}
	for _, file := range files {
		if err := os.WriteFile(filepath.Join(module, file.name), []byte(file.text), 0o600); err != nil {
			return "", fmt.Errorf("write the checker build module: %w", err)
		}
	}
	name := checkerBinary
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	binary := filepath.Join(dir, name)
	ctx, cancel := context.WithTimeout(ctx, installTimeout)
	defer cancel()
	// #nosec G204 -- the arguments are constants; no shell runs.
	build := exec.CommandContext(ctx, "go", "build", "-mod=readonly", "-o", binary, checkerPackage)
	build.Dir = module
	build.Stdout, build.Stderr = stderr, stderr
	if err := build.Run(); err != nil {
		return "", fmt.Errorf("build %s from the pinned build module: %w", checkerPackage, err)
	}
	return binary, nil
}

// verifyChecker runs the checker on a canary repository whose second commit removes an
// exported function, and fails unless it reports that change as incompatible. A checker built
// against a golang.org/x/tools too old for the running toolchain's export data reads every
// package as empty and passes every module; the canary turns that silent pass into a failure.
func verifyChecker(ctx context.Context, checker string, stderr io.Writer) (err error) {
	dir, err := os.MkdirTemp("", "apicompat-canary-")
	if err != nil {
		return fmt.Errorf("create the canary repository: %w", err)
	}
	defer func() { err = errors.Join(err, os.RemoveAll(dir)) }()
	repo := filepath.Join(dir, "repo")
	base, head, err := commitCanary(ctx, repo, filepath.Join(dir, "no-hooks"))
	if err != nil {
		return fmt.Errorf("create the canary repository: %w", err)
	}
	incompatible, err := runChecker(ctx, checker, repo, repo, base, head, io.Discard, stderr)
	if err != nil {
		return fmt.Errorf("the checker failed on its canary: %w", err)
	}
	if !incompatible {
		return errors.New("the checker reported the canary's removed exported function as compatible, " +
			"so it cannot read the packages this Go toolchain builds")
	}
	return nil
}

// commitCanary creates the canary repository in repo with two commits and returns them. The
// commits run under scratchGitConfig.
func commitCanary(ctx context.Context, repo, hooks string) (base, head string, err error) {
	if err := os.MkdirAll(repo, 0o700); err != nil {
		return "", "", err
	}
	commit := slices.Clip(append(scratchGitConfig(hooks), "commit", "--quiet", "--all", "--message"))
	steps := []func() error{
		func() error { return runTool(ctx, repo, io.Discard, io.Discard, "git", "init", "--quiet") },
		func() error { return writeCanary(repo, canaryBefore) },
		func() error { return runTool(ctx, repo, io.Discard, io.Discard, "git", "add", "--all") },
		func() error { return runTool(ctx, repo, io.Discard, io.Discard, "git", append(commit, "base")...) },
		func() error { base, err = commitOf(ctx, repo, "HEAD"); return err },
		func() error { return os.WriteFile(filepath.Join(repo, "canary.go"), []byte(canaryAfter), 0o600) },
		func() error { return runTool(ctx, repo, io.Discard, io.Discard, "git", append(commit, "head")...) },
		func() error { head, err = commitOf(ctx, repo, "HEAD"); return err },
	}
	for index := 0; index < len(steps); index++ {
		if err := steps[index](); err != nil {
			return "", "", err
		}
	}
	return base, head, nil
}

// writeCanary writes the canary module's go.mod and its source.
func writeCanary(repo, source string) error {
	if err := os.WriteFile(filepath.Join(repo, "go.mod"), []byte(canaryGoMod), 0o600); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(repo, "canary.go"), []byte(source), 0o600)
}

// runChecker compares the module in moduleDir between base and head of the repository at root,
// and reports whether the checker found an incompatible change.
func runChecker(ctx context.Context, checker, root, moduleDir, base, head string, stdout, stderr io.Writer) (bool, error) {
	ctx, cancel := context.WithTimeout(ctx, moduleTimeout)
	defer cancel()
	diagnostics := &boundedBuffer{limit: maxDiagnostics}
	// #nosec G204 -- the checker is the binary this run installed or the operator's -checker; base and head are resolved commit names; no shell runs.
	command := exec.CommandContext(ctx, checker, base, head, "--repo-path="+root)
	command.Dir = moduleDir
	command.Stdout = stdout
	command.Stderr = io.MultiWriter(stderr, diagnostics)
	return classifyChecker(command.Run(), diagnostics.String())
}

// classifyChecker reads one checker run: status 0 is compatible unless the run printed an
// error, status 1 is incompatible, and anything else is a comparison that did not run.
func classifyChecker(runErr error, diagnostics string) (bool, error) {
	var exit *exec.ExitError
	switch {
	case runErr == nil && reportedError(diagnostics):
		return false, errors.New("the checker printed an error and exited 0, so it compared nothing")
	case runErr == nil:
		return false, nil
	case errors.As(runErr, &exit) && exit.ExitCode() == exitRejected:
		return true, nil
	default:
		return false, fmt.Errorf("the checker failed: %w", runErr)
	}
}

// reportedError reports whether diagnostics hold a line in cobra's error format.
func reportedError(diagnostics string) bool {
	lines := strings.Split(diagnostics, "\n")
	for index := 0; index < len(lines) && index < maxGoModLines; index++ {
		if strings.HasPrefix(strings.TrimSpace(lines[index]), "Error: ") {
			return true
		}
	}
	return false
}

// verdict prints every module's outcome and the policy, and returns the exit status.
func verdict(repo repository, plan modulePlan, results []result, stdout, stderr *printer) int {
	if plan.empty() {
		stdout.printf("No Go module is tracked at %s or HEAD %s: nothing to compare.\n", repo.baseName, short(repo.head))
		return exitCompatible
	}
	stdout.printf("\nGo API compatibility from %s (%s) to HEAD %s:\n", repo.baseName, short(repo.base), short(repo.head))
	incompatible := len(plan.removed)
	for index := 0; index < len(results) && index < maxModules; index++ {
		if results[index].notCompared != "" {
			printModule(stdout, "not compared", results[index].module, results[index].notCompared)
			continue
		}
		state := "compatible"
		if results[index].incompatible {
			state = "incompatible"
			incompatible++
		}
		printModule(stdout, state, results[index].module, "")
	}
	for index := 0; index < len(plan.newMajor) && index < maxModules; index++ {
		target := plan.newMajor[index]
		printModule(stdout, "new major", target, "a new major version of "+target.from+", nothing to compare")
	}
	printModules(stdout, "added", plan.added, "new at HEAD, nothing to compare")
	printModules(stdout, "removed", plan.removed, "every package of the module is gone")
	printModules(stdout, "not public", plan.notPublic, "the module path has an internal element")
	if incompatible == 0 {
		stdout.printf("Every compared module is compatible (policy %s).\n", repo.policy.reason)
		return exitCompatible
	}
	message := fmt.Sprintf("%d incompatible module change(s) from %s to HEAD %s", incompatible, repo.baseName, short(repo.head))
	if repo.policy.reject {
		stderr.annotate("error", message+" rejected ("+repo.policy.reason+")")
		return exitRejected
	}
	stdout.annotate("warning", message+" reported, not rejected ("+repo.policy.reason+")")
	return exitCompatible
}

func printModules(stdout *printer, state string, modules []module, note string) {
	for index := 0; index < len(modules) && index < maxModules; index++ {
		printModule(stdout, state, modules[index], note)
	}
}

func printModule(stdout *printer, state string, target module, note string) {
	if note != "" {
		note = ": " + note
	}
	stdout.printf("  %-12s %s (%s)%s\n", state, target.dir, target.path, note)
}

// short abbreviates a commit name for display.
func short(commit string) string {
	if len(commit) > 12 {
		return commit[:12]
	}
	return commit
}

// runTool runs name args in dir under moduleTimeout.
func runTool(ctx context.Context, dir string, stdout, stderr io.Writer, name string, args ...string) error {
	ctx, cancel := context.WithTimeout(ctx, moduleTimeout)
	defer cancel()
	// #nosec G204 -- callers pass the go or git command and fixed arguments; no shell runs.
	command := exec.CommandContext(ctx, name, args...)
	command.Dir = dir
	command.Stdout, command.Stderr = stdout, stderr
	if err := command.Run(); err != nil {
		return fmt.Errorf("%s %s: %w", name, strings.Join(args, " "), err)
	}
	return nil
}

// gitCapture runs git in dir and returns its standard output, refusing more than limit bytes.
func gitCapture(ctx context.Context, dir string, limit int, args ...string) ([]byte, error) {
	var out []byte
	err := runGit(ctx, dir, func(stdout io.Reader) error {
		data, err := io.ReadAll(io.LimitReader(stdout, int64(limit)+1))
		if err == nil && len(data) > limit {
			err = errOutputLimit
		}
		out = data
		return err
	}, args...)
	return out, err
}

// gitOutput runs git in dir and returns its standard output, at most maxRevisionBytes, without
// surrounding white space: a commit, tree or object name, or nothing.
func gitOutput(ctx context.Context, dir string, args ...string) (string, error) {
	out, err := gitCapture(ctx, dir, maxRevisionBytes, args...)
	return strings.TrimSpace(string(out)), err
}

// gitRecords runs git in dir and returns the records of its output, split at sep, that keep
// selects. More than limit records fail rather than truncate the list.
func gitRecords(ctx context.Context, dir string, sep byte, limit int, keep func(string) bool, args ...string) ([]string, error) {
	var kept []string
	err := runGit(ctx, dir, func(stdout io.Reader) error {
		var err error
		kept, err = scanRecords(bufio.NewReader(stdout), sep, limit, keep)
		return err
	}, args...)
	return kept, err
}

// scanRecords reads at most limit records ending in sep; a final record without sep counts.
func scanRecords(reader *bufio.Reader, sep byte, limit int, keep func(string) bool) ([]string, error) {
	var kept []string
	for count := 0; count <= limit; count++ {
		record, err := reader.ReadString(sep)
		if err != nil && !errors.Is(err, io.EOF) {
			return nil, err
		}
		if len(record) > maxRecordBytes {
			return nil, fmt.Errorf("an output record exceeds %d bytes", maxRecordBytes)
		}
		if record = strings.TrimSuffix(record, string(sep)); record != "" && keep(record) {
			kept = append(kept, record)
		}
		if errors.Is(err, io.EOF) {
			return kept, nil
		}
	}
	return nil, fmt.Errorf("output exceeds %d records", limit)
}

// runGit runs git -C dir args under gitTimeout and hands its standard output to consume. When
// consume fails, git is stopped; standard error, bounded, is carried in a failure.
func runGit(ctx context.Context, dir string, consume func(io.Reader) error, args ...string) error {
	ctx, cancel := context.WithTimeout(ctx, gitTimeout)
	defer cancel()
	diagnostics := &boundedBuffer{limit: maxDiagnostics}
	// #nosec G204 -- git with the gate's fixed subcommands; a revision argument never starts with "-" (parseOptions) and no shell runs.
	command := exec.CommandContext(ctx, "git", append([]string{"-C", dir}, args...)...)
	command.Stderr = diagnostics
	stdout, err := command.StdoutPipe()
	if err != nil {
		return fmt.Errorf("git %s: %w", args[0], err)
	}
	if err := command.Start(); err != nil {
		return fmt.Errorf("git %s: %w", args[0], err)
	}
	if err := consume(stdout); err != nil {
		cancel()
		return errors.Join(fmt.Errorf("git %s: %w", args[0], err), command.Wait())
	}
	if err := command.Wait(); err != nil {
		return fmt.Errorf("git %s: %w: %s", strings.Join(args, " "), err, strings.TrimSpace(diagnostics.String()))
	}
	return nil
}

// boundedBuffer keeps the first limit bytes written to it and drops the rest, reporting every
// write as complete so a writer sharing an io.MultiWriter with it still receives all output.
type boundedBuffer struct {
	data  []byte
	limit int
}

func (b *boundedBuffer) Write(p []byte) (int, error) {
	if room := b.limit - len(b.data); room > 0 {
		b.data = append(b.data, p[:min(room, len(p))]...)
	}
	return len(p), nil
}

func (b *boundedBuffer) String() string {
	return string(b.data)
}
