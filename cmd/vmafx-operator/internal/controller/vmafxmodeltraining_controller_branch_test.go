// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxmodeltraining_controller_branch_test.go —
// VmafxModelTraining reconciler branch-coverage tests.
//
// Complements vmafxmodeltraining_controller_test.go by exercising the two
// branches the happy-path suite does not reach:
//
//   - Not-found request: a deleted (or never-created) VmafxModelTraining
//     reconciles to (Result{}, nil) via client.IgnoreNotFound.
//   - Already-initialised: when Phase is already Running the reconciler must
//     leave Phase untouched and still request the 60s requeue.
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

var _ = Describe("VmafxModelTraining controller — branch coverage", func() {
	ctx := context.Background()

	It("returns a clean no-op when the VmafxModelTraining does not exist", func() {
		reconciler := &controller.VmafxModelTrainingReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{
			NamespacedName: types.NamespacedName{
				Name:      "never-created-training",
				Namespace: "default",
			},
		})
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(BeZero())
	})

	It("preserves Phase when the training run is already past Initializing", func() {
		mt := &vmafxv1.VmafxModelTraining{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-training-running",
				Namespace: "default",
			},
			Spec: vmafxv1.VmafxModelTrainingSpec{
				BaseModel:      "vmaf_v0.6.1",
				Algorithm:      "online-sgd-ema",
				OutputRegistry: "ghcr.io/vmafx/models",
				DataSource:     vmafxv1.VmafxModelTrainingDataSource{},
			},
		}
		Expect(k8sClient.Create(ctx, mt)).To(Succeed())

		// Advance to Running with a non-zero sample count, simulating a
		// run that has been live for a while.
		nn := types.NamespacedName{Name: mt.Name, Namespace: mt.Namespace}
		var fetched vmafxv1.VmafxModelTraining
		Expect(k8sClient.Get(ctx, nn, &fetched)).To(Succeed())
		fetched.Status.Phase = vmafxv1.VmafxModelTrainingPhaseRunning
		fetched.Status.CurrentSamples = int32(42)
		fetched.Status.ModelVersion = "sha256:deadbeef"
		Expect(k8sClient.Status().Update(ctx, &fetched)).To(Succeed())

		reconciler := &controller.VmafxModelTrainingReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		result, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())
		Expect(result.RequeueAfter).To(Equal(60*time.Second),
			"every reconcile pass must re-arm the 60s requeue, terminal or not")

		// Confirm the reconciler did not regress any status field.
		var after vmafxv1.VmafxModelTraining
		Expect(k8sClient.Get(ctx, nn, &after)).To(Succeed())
		Expect(after.Status.Phase).To(Equal(vmafxv1.VmafxModelTrainingPhaseRunning))
		Expect(after.Status.CurrentSamples).To(Equal(int32(42)))
		Expect(after.Status.ModelVersion).To(Equal("sha256:deadbeef"))
	})
})
