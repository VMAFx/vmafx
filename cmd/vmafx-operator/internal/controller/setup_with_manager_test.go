// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/setup_with_manager_test.go —
// SetupWithManager wiring tests.
//
// Verifies that each of the three reconcilers can be registered with a
// controller-runtime manager without error. SetupWithManager is the surface
// invoked from main.go at operator startup; failure there bricks the whole
// operator. The original Stage 1 suite exercised only Reconcile() directly,
// leaving this surface at 0% coverage.
//
// The test does NOT call mgr.Start — registration is sufficient to validate
// the reconciler-controller plumbing.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.

package controller_test

import (
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	ctrl "sigs.k8s.io/controller-runtime"
	"sigs.k8s.io/controller-runtime/pkg/manager"
	metricsserver "sigs.k8s.io/controller-runtime/pkg/metrics/server"

	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/controller"
)

var _ = Describe("SetupWithManager wiring", func() {
	// newTestManager builds a controller-runtime manager bound to the envtest
	// rest.Config. The metrics server is disabled (BindAddress: "0") so the
	// per-spec manager does not collide on the default :8080.
	newTestManager := func() manager.Manager {
		mgr, err := ctrl.NewManager(cfg, ctrl.Options{
			Scheme:  scheme,
			Metrics: metricsserver.Options{BindAddress: "0"},
		})
		Expect(err).NotTo(HaveOccurred())
		return mgr
	}

	It("registers the VmafxJob reconciler with the manager", func() {
		mgr := newTestManager()
		r := &controller.VmafxJobReconciler{
			Client: mgr.GetClient(),
			Scheme: mgr.GetScheme(),
		}
		Expect(r.SetupWithManager(mgr)).To(Succeed())
	})

	It("registers the VmafxNode reconciler with the manager", func() {
		mgr := newTestManager()
		r := &controller.VmafxNodeReconciler{
			Client: mgr.GetClient(),
			Scheme: mgr.GetScheme(),
		}
		Expect(r.SetupWithManager(mgr)).To(Succeed())
	})

	It("registers the VmafxModelTraining reconciler with the manager", func() {
		mgr := newTestManager()
		r := &controller.VmafxModelTrainingReconciler{
			Client: mgr.GetClient(),
			Scheme: mgr.GetScheme(),
		}
		Expect(r.SetupWithManager(mgr)).To(Succeed())
	})
})
