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

	It("advances Phase to Running when AssignedNode is set", func() {
		job := &vmafxv1.VmafxJob{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-job-running",
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

		// First reconcile: Pending.
		_, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())

		// Simulate the controller assigning a node.
		var pending vmafxv1.VmafxJob
		Expect(k8sClient.Get(ctx, nn, &pending)).To(Succeed())
		pending.Status.AssignedNode = "vmafx-node-0"
		Expect(k8sClient.Status().Update(ctx, &pending)).To(Succeed())

		// Second reconcile: Running.
		_, err = reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())

		var running vmafxv1.VmafxJob
		Eventually(func() vmafxv1.VmafxJobPhase {
			_ = k8sClient.Get(ctx, nn, &running)
			return running.Status.Phase
		}, timeout, interval).Should(Equal(vmafxv1.VmafxJobPhaseRunning))
	})
})
