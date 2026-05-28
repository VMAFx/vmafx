// SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
// Copyright 2026 Lusoris
//
// cmd/vmafx-operator/internal/controller/vmafxmodeltraining_controller_test.go — VmafxModelTraining reconciler tests.
//
// Verifies that a new VmafxModelTraining is initialised with Phase == Initializing.
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

var _ = Describe("VmafxModelTraining controller", func() {
	const (
		timeout  = 10 * time.Second
		interval = 250 * time.Millisecond
	)

	ctx := context.Background()

	It("sets Phase to Initializing on a new VmafxModelTraining", func() {
		mt := &vmafxv1.VmafxModelTraining{
			ObjectMeta: metav1.ObjectMeta{
				Name:      "test-training",
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

		reconciler := &controller.VmafxModelTrainingReconciler{
			Client: k8sClient,
			Scheme: scheme,
		}
		nn := types.NamespacedName{Name: mt.Name, Namespace: mt.Namespace}

		_, err := reconciler.Reconcile(ctx, ctrl.Request{NamespacedName: nn})
		Expect(err).NotTo(HaveOccurred())

		var updated vmafxv1.VmafxModelTraining
		Eventually(func() vmafxv1.VmafxModelTrainingPhase {
			_ = k8sClient.Get(ctx, nn, &updated)
			return updated.Status.Phase
		}, timeout, interval).Should(Equal(vmafxv1.VmafxModelTrainingPhaseInitializing))
	})
})
