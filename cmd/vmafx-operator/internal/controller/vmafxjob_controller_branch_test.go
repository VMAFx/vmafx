// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxjob_controller_branch_test.go —
// VmafxJob reconciler branch-coverage tests.
//
// Complements vmafxjob_controller_test.go by exercising the two branches the
// happy-path suite does not reach:
//
//   - Not-found request: a deleted (or never-created) VmafxJob must reconcile
//     to (Result{}, nil) via client.IgnoreNotFound.
//   - Terminal phase: a job already in Succeeded must not have its Phase
//     mutated by another reconcile pass — Stage 1 only initialises or
//     advances Pending->Running.
//
// ADR-0714: vmafx-operator kubebuilder skeleton + CRDs.

package controller_test

import (
	"context"

	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	ctrl "sigs.k8s.io/controller-runtime"

	vmafxv1 "github.com/VMAFx/vmafx/api/vmafx/v1"
	"github.com/VMAFx/vmafx/cmd/vmafx-operator/internal/controller"
)

var _ = Describe("VmafxJob controller — branch coverage", func() {
	ctx := context.Background()

	It("returns a clean no-op when the VmafxJob does not exist", func() {
		reconciler := &controller.VmafxJobReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{
			NamespacedName: types.NamespacedName{
				Name:      "never-created-job",
				Namespace: "default",
			},
		})
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(BeZero())
		Expect(result.Requeue).To(BeFalse())
	})

	It("does not mutate a VmafxJob that is already in a terminal phase", func() {
		job := &vmafxv1.VmafxJob{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-job-terminal",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxJobSpec{
				Reference: "file:///ref.yuv",
				Distorted: "file:///dis.yuv",
			},
		}
		Expect(k8sClient.Create(ctx, job)).To(Succeed())

		// Promote straight to Succeeded with a populated score, simulating a
		// job whose Pod has already terminated and been reconciled by a
		// future-stage Pod-watcher.
		var fetched vmafxv1.VmafxJob
		nn := types.NamespacedName{Name: job.Name, Namespace: job.Namespace}
		Expect(k8sClient.Get(ctx, nn, &fetched)).To(Succeed())
		fetched.Status.Phase = vmafxv1.VmafxJobPhaseSucceeded
		fetched.Status.Score = 95.5
		fetched.Status.AssignedNode = "vmafx-node-A"
		Expect(k8sClient.Status().Update(ctx, &fetched)).To(Succeed())

		reconciler := &controller.VmafxJobReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(BeZero(),
			"terminal jobs must not be re-queued in Stage 1")

		// Confirm the reconciler did not regress Phase or Score.
		var after vmafxv1.VmafxJob
		Expect(k8sClient.Get(ctx, nn, &after)).To(Succeed())
		Expect(after.Status.Phase).To(Equal(vmafxv1.VmafxJobPhaseSucceeded))
		Expect(after.Status.Score).To(Equal(95.5))
		Expect(after.Status.AssignedNode).To(Equal("vmafx-node-A"))
	})
})
