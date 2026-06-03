// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxjob_controller_test.go — VmafxJob reconciler tests.
//
// Stage 1 tests verify that:
//   - A new VmafxJob with no Phase gets its Phase set to Pending.
//   - A VmafxJob with AssignedNode set advances from Pending to Running.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.

package controller_test

import (
	"context"
	"time"

	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	ctrl "sigs.k8s.io/controller-runtime"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/controller"
)

var _ = Describe("VmafxJob controller", func() {
	const (
		timeout  = 10 * time.Second
		interval = 250 * time.Millisecond
	)

	ctx := context.Background()

	It("sets Phase to Pending on a new VmafxJob", func() {
		job := &vmafxv1.VmafxJob{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-job-pending",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxJobSpec{
				Reference: "file:///ref.yuv",
				Distorted: "file:///dis.yuv",
			},
		}
		Expect(k8sClient.Create(ctx, job)).To(Succeed())

		reconciler := &controller.VmafxJobReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		_, err := reconciler.Reconcile(ctx, ctrl.Request{
			NamespacedName: types.NamespacedName{
				Name:      job.Name,
				Namespace: job.Namespace,
			},
		})
		Expect(err).NotTo(HaveOccurred())

		var updated vmafxv1.VmafxJob
		Eventually(func() vmafxv1.VmafxJobPhase {
			_ = k8sClient.Get(ctx, types.NamespacedName{
				Name:      job.Name,
				Namespace: job.Namespace,
			}, &updated)
			return updated.Status.Phase
		}, timeout, interval).Should(Equal(vmafxv1.VmafxJobPhasePending))
	})

	It("stays Pending and requeues when ControllerJobID is not set", func() {
		// Stage 2: the operator waits for the external scheduler to set ControllerJobID.
		// Until then the job stays Pending and is requeued after jobPollInterval.
		job := &vmafxv1.VmafxJob{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-job-no-id",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxJobSpec{
				Reference: "file:///ref.yuv",
				Distorted: "file:///dis.yuv",
			},
		}
		Expect(k8sClient.Create(ctx, job)).To(Succeed())

		reconciler := &controller.VmafxJobReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		nn := types.NamespacedName{Name: job.Name, Namespace: job.Namespace}

		// First reconcile: sets Pending.
		result, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(BeNumerically(">", 0),
			"should requeue because ControllerJobID is not set")

		// Second reconcile: still Pending, no gRPC dial attempted.
		_, err = reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())

		var updated vmafxv1.VmafxJob
		Expect(k8sClient.Get(ctx, nn, &updated)).To(Succeed())
		Expect(updated.Status.Phase).To(Equal(vmafxv1.VmafxJobPhasePending))
		Expect(updated.Status.ControllerJobID).To(BeEmpty())
	})

	It("does not requeue terminal (Succeeded) jobs", func() {
		job := &vmafxv1.VmafxJob{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-job-succeeded",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxJobSpec{
				Reference: "file:///ref.yuv",
				Distorted: "file:///dis.yuv",
			},
		}
		Expect(k8sClient.Create(ctx, job)).To(Succeed())

		// Pre-set a terminal phase.
		job.Status.Phase = vmafxv1.VmafxJobPhaseSucceeded
		Expect(k8sClient.Status().Update(ctx, job)).To(Succeed())

		reconciler := &controller.VmafxJobReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		nn := types.NamespacedName{Name: job.Name, Namespace: job.Namespace}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(BeZero(), "terminal jobs must not be requeued")
	})
})
