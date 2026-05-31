// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxnode_controller_test.go — VmafxNode reconciler tests.
//
// Covers reconciliation paths the original Stage 1 suite skipped:
//
//   - Happy path: an injected HTTPClient that resolves /healthz to 200 OK
//     causes Status.Healthy to flip to true and LastHeartbeat to be stamped.
//   - Unhealthy path: a transport-level error (non-routable URL via a faulty
//     RoundTripper) leaves Healthy=false but still stamps LastHeartbeat and
//     requests a 30s requeue (the reconciler never aborts on probe failure).
//   - Not-found path: reconciling a deleted VmafxNode returns (Result{}, nil)
//     via client.IgnoreNotFound.
//
// All three flows produce 0% coverage in the baseline suite (envtest only
// exercises the Job + ModelTraining reconcilers). With these tests the
// VmafxNode reconciler reaches the same coverage tier as its peers.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.
// ADR-0108: deep-dive deliverables rule (test plan documented in PR body).

package controller_test

import (
	"context"
	"errors"
	"net/http"
	"time"

	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	ctrl "sigs.k8s.io/controller-runtime"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/controller"
)

// stubRoundTripper is a minimal http.RoundTripper whose response is whatever
// the test sets in `resp` / `err`. It bypasses the operator's hard-coded
// vmafx-controller.<ns>.svc.cluster.local URL — the reconciler never actually
// dials a network address.
type stubRoundTripper struct {
	resp *http.Response
	err  error
}

func (s *stubRoundTripper) RoundTrip(_ *http.Request) (*http.Response, error) {
	if s.err != nil {
		return nil, s.err
	}
	return s.resp, nil
}

var _ = Describe("VmafxNode controller", func() {
	const (
		timeout  = 10 * time.Second
		interval = 250 * time.Millisecond
	)

	ctx := context.Background()

	It("sets Healthy=true and stamps LastHeartbeat when /healthz returns 200", func() {
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

		// Inject an HTTPClient that always returns HTTP 200 — no real network.
		reconciler := &controller.VmafxNodeReconciler{
			Client: k8sClient,
			Scheme: scheme,
			HTTPClient: &http.Client{
				Transport: &stubRoundTripper{
					resp: &http.Response{
						StatusCode: http.StatusOK,
						Body:       http.NoBody,
					},
				},
				Timeout: 2 * time.Second,
			},
		}

		nn := types.NamespacedName{Name: node.Name, Namespace: node.Namespace}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())
		// Reconciler always requests a 30s requeue regardless of health outcome.
		Expect(result.RequeueAfter).To(Equal(30 * time.Second))

		var updated vmafxv1.VmafxNode
		Eventually(func() bool {
			_ = k8sClient.Get(ctx, nn, &updated)
			return updated.Status.Healthy
		}, timeout, interval).Should(BeTrue())
		Expect(updated.Status.LastHeartbeat).NotTo(BeNil(),
			"reconciler must stamp LastHeartbeat on every probe")
	})

	It("sets Healthy=false but still requeues when /healthz transport errors", func() {
		node := &vmafxv1.VmafxNode{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-node-unhealthy",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxNodeSpec{
				GPUVendor: "cpu",
				Capacity:  2,
			},
		}
		Expect(k8sClient.Create(ctx, node)).To(Succeed())

		// Inject an HTTPClient whose RoundTripper always returns an error,
		// simulating a node whose /healthz endpoint is unreachable.
		reconciler := &controller.VmafxNodeReconciler{
			Client: k8sClient,
			Scheme: scheme,
			HTTPClient: &http.Client{
				Transport: &stubRoundTripper{
					err: errors.New("simulated transport failure"),
				},
				Timeout: 2 * time.Second,
			},
		}

		nn := types.NamespacedName{Name: node.Name, Namespace: node.Namespace}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		// A failed probe is NOT an error — the reconciler only returns err
		// when the status subresource write itself fails.
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(Equal(30*time.Second),
			"reconciler must keep polling even after a failed probe")

		var updated vmafxv1.VmafxNode
		Eventually(func() bool {
			_ = k8sClient.Get(ctx, nn, &updated)
			// Healthy must remain false AND LastHeartbeat must still be set.
			return updated.Status.LastHeartbeat != nil
		}, timeout, interval).Should(BeTrue())
		Expect(updated.Status.Healthy).To(BeFalse(),
			"transport error must surface as Healthy=false")
	})

	It("returns Result{} with no error when the VmafxNode does not exist", func() {
		// Deletion path: reconciling a name that was never created (or has
		// been garbage-collected) must be a clean no-op via IgnoreNotFound.
		reconciler := &controller.VmafxNodeReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{
			NamespacedName: types.NamespacedName{
				Name:      "never-created-node",
				Namespace: "default",
			},
		})
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(BeZero(),
			"a not-found object must not be re-queued")
		Expect(result.Requeue).To(BeFalse())
	})
})
