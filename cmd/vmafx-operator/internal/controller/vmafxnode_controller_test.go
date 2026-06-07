// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxnode_controller_test.go — VmafxNode reconciler tests.
//
// Tests verify:
//   - A healthy probe → Healthy = true.
//   - A failing probe → Healthy = false.
//   - A stale heartbeat (LastHeartbeat > 60 s ago) → Healthy = false even when the
//     probe would otherwise succeed.
//   - The operator does NOT overwrite LastHeartbeat; that field is owned exclusively
//     by the node agent via the controller's Heartbeat RPC (ADR-1069).
//
// A real HTTP server is spun up per test to simulate the vmafx-controller /healthz
// endpoint; the reconciler's ControllerHTTPAddr field is injected to override DNS.
//
// ADR-0786: vmafx-operator Stage 2 — envtest suite.
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs (parent).
// ADR-1069: VmafxNode LastHeartbeat ownership — node agent only.

package controller_test

import (
	"context"
	"net/http"
	"net/http/httptest"
	"time"

	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	ctrl "sigs.k8s.io/controller-runtime"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/controller"
)

var _ = Describe("VmafxNode controller", func() {
	const (
		timeout  = 10 * time.Second
		interval = 250 * time.Millisecond
	)

	ctx := context.Background()

	It("sets Healthy = true when the controller /healthz returns 200", func() {
		// Spin up a fake controller that always returns 200.
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusOK)
		}))
		DeferCleanup(srv.Close)

		node := &vmafxv1.VmafxNode{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-node-healthy",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxNodeSpec{
				GPUVendor: "nvidia",
				Capacity:  1,
			},
		}
		Expect(k8sClient.Create(ctx, node)).To(Succeed())

		reconciler := &controller.VmafxNodeReconciler{
			Client:             k8sClient,
			Scheme:             scheme,
			ControllerHTTPAddr: srv.URL,
		}
		nn := types.NamespacedName{Name: node.Name, Namespace: node.Namespace}
		_, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())

		var updated vmafxv1.VmafxNode
		Eventually(func() bool {
			_ = k8sClient.Get(ctx, nn, &updated)
			return updated.Status.Healthy
		}, timeout, interval).Should(BeTrue())
		// The operator does NOT write LastHeartbeat — that field is owned by the
		// node agent (ADR-1069). A newly-created node without a pre-set
		// LastHeartbeat must have nil here.
		Expect(updated.Status.LastHeartbeat).To(BeNil())
	})

	It("sets Healthy = false when the controller /healthz returns non-200", func() {
		// Fake controller returns 503.
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusServiceUnavailable)
		}))
		DeferCleanup(srv.Close)

		node := &vmafxv1.VmafxNode{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-node-unhealthy",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxNodeSpec{
				GPUVendor: "amd",
				Capacity:  2,
			},
		}
		Expect(k8sClient.Create(ctx, node)).To(Succeed())

		reconciler := &controller.VmafxNodeReconciler{
			Client:             k8sClient,
			Scheme:             scheme,
			ControllerHTTPAddr: srv.URL,
		}
		nn := types.NamespacedName{Name: node.Name, Namespace: node.Namespace}
		_, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())

		var updated vmafxv1.VmafxNode
		Eventually(func() bool {
			_ = k8sClient.Get(ctx, nn, &updated)
			return !updated.Status.Healthy
		}, timeout, interval).Should(BeTrue(),
			"node with failing /healthz probe should be marked unhealthy")
		// The operator does NOT write LastHeartbeat (ADR-1069); it remains nil
		// for a node that has never had a heartbeat from its agent.
		Expect(updated.Status.LastHeartbeat).To(BeNil())
	})

	It("sets Healthy = false when LastHeartbeat is stale (>60 s)", func() {
		// Fake controller returns 200 — but the heartbeat is stale.
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
			w.WriteHeader(http.StatusOK)
		}))
		DeferCleanup(srv.Close)

		staleTime := metav1.NewTime(time.Now().Add(-90 * time.Second))
		node := &vmafxv1.VmafxNode{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-node-stale",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxNodeSpec{
				GPUVendor: "intel",
			},
		}
		Expect(k8sClient.Create(ctx, node)).To(Succeed())

		// Manually pre-set a stale LastHeartbeat via status subresource.
		node.Status.Healthy = true
		node.Status.LastHeartbeat = &staleTime
		Expect(k8sClient.Status().Update(ctx, node)).To(Succeed())

		reconciler := &controller.VmafxNodeReconciler{
			Client:             k8sClient,
			Scheme:             scheme,
			ControllerHTTPAddr: srv.URL,
		}
		nn := types.NamespacedName{Name: node.Name, Namespace: node.Namespace}
		_, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())

		var updated vmafxv1.VmafxNode
		// The operator does NOT modify LastHeartbeat (ADR-1069); it must remain
		// unchanged at the stale value set by the node agent.
		Eventually(func() bool {
			_ = k8sClient.Get(ctx, nn, &updated)
			return !updated.Status.Healthy
		}, timeout, interval).Should(BeTrue(),
			"node should be unhealthy because the stored heartbeat was stale")
		Expect(updated.Status.LastHeartbeat).To(Equal(&staleTime),
			"LastHeartbeat must not be overwritten by the operator — it is owned by the node agent (ADR-1069)")
	})
})
