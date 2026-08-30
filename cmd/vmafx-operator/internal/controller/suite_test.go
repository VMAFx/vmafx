// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/suite_test.go — envtest suite bootstrap.
//
// This file sets up the controller-runtime envtest environment that all
// reconciler tests in this package share.  It installs the vmafx.dev/v1 CRDs,
// starts the API server + etcd embedded in envtest, and tears them down after
// the suite completes.
//
// To run the tests you need the envtest binaries installed:
//
//	go install sigs.k8s.io/controller-runtime/tools/setup-envtest@latest
//	export KUBEBUILDER_ASSETS=$(setup-envtest use 1.31 -p path)
//	go test ./cmd/vmafx-operator/internal/controller/...
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.

package controller_test

import (
	"log/slog"
	"os"
	"path/filepath"
	"testing"

	"github.com/go-logr/logr"
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	"k8s.io/apimachinery/pkg/runtime"
	clientgoscheme "k8s.io/client-go/kubernetes/scheme"
	"k8s.io/client-go/rest"
	"sigs.k8s.io/controller-runtime/pkg/client"
	"sigs.k8s.io/controller-runtime/pkg/envtest"
	logf "sigs.k8s.io/controller-runtime/pkg/log"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
)

// envtestSkipMessage is emitted when KUBEBUILDER_ASSETS is unset. It explains
// to the developer exactly how to install the envtest control-plane binaries
// (etcd + kube-apiserver + kubectl) so the suite can run locally.
//
// The CI workflow (.github/workflows/go-ci.yml) and `make setup-envtest`
// arrange this automatically; this skip path is defense in depth for ad-hoc
// `go test` invocations from a fresh checkout where the assets are missing.
const envtestSkipMessage = "skipping envtest suite: KUBEBUILDER_ASSETS is unset. " +
	"Run `make setup-envtest` (or `go install sigs.k8s.io/controller-runtime/tools/setup-envtest@latest && " +
	"export KUBEBUILDER_ASSETS=$(setup-envtest use 1.31 -p path)`) and re-run."

var (
	cfg       *rest.Config
	k8sClient client.Client
	testEnv   *envtest.Environment
	scheme    = runtime.NewScheme()
)

func TestControllers(t *testing.T) {
	if os.Getenv("KUBEBUILDER_ASSETS") == "" {
		t.Skip(envtestSkipMessage)
	}
	RegisterFailHandler(Fail)
	RunSpecs(t, "vmafx-operator controller suite")
}

var _ = BeforeSuite(func() {
	// Test logger: slog text handler at debug level, routed to GinkgoWriter so
	// the test runner interleaves log output with spec output.
	handler := slog.NewTextHandler(GinkgoWriter, &slog.HandlerOptions{
		Level: slog.LevelDebug,
	})
	logf.SetLogger(logr.FromSlogHandler(handler))

	// CRD manifests are in config/crd/bases/ relative to the repo root.
	// The envtest runner expects absolute paths; filepath.Join resolves from
	// the test binary's working directory.
	testEnv = &envtest.Environment{
		CRDDirectoryPaths: []string{
			filepath.Join("..", "..", "..", "..", "config", "crd", "bases"),
		},
		ErrorIfCRDPathMissing: true,
	}

	var err error
	cfg, err = testEnv.Start()
	Expect(err).NotTo(HaveOccurred())
	Expect(cfg).NotTo(BeNil())

	Expect(clientgoscheme.AddToScheme(scheme)).To(Succeed())
	Expect(vmafxv1.AddToScheme(scheme)).To(Succeed())

	k8sClient, err = client.New(cfg, client.Options{Scheme: scheme})
	Expect(err).NotTo(HaveOccurred())
	Expect(k8sClient).NotTo(BeNil())
})

var _ = AfterSuite(func() {
	// testEnv is nil only when TestControllers skipped before BeforeSuite ran.
	// Guard against the nil-deref panic noted in PRs #330 / #341 / #362.
	if testEnv == nil {
		return
	}
	Expect(testEnv.Stop()).To(Succeed())
})
