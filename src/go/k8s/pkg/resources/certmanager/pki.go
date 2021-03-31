// Copyright 2021 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

// Package certmanager contains resources for TLS certificate handling using cert-manager
package certmanager

import (
	"context"

	"github.com/go-logr/logr"
	cmmetav1 "github.com/jetstack/cert-manager/pkg/apis/meta/v1"
	redpandav1alpha1 "github.com/vectorizedio/redpanda/src/go/k8s/apis/redpanda/v1alpha1"
	"github.com/vectorizedio/redpanda/src/go/k8s/pkg/resources"
	"k8s.io/apimachinery/pkg/runtime"
	"k8s.io/apimachinery/pkg/types"
	k8sclient "sigs.k8s.io/controller-runtime/pkg/client"
)

var _ resources.Reconciler = &PkiReconciler{}

// RootCert cert name
const RootCert = "rootcert"

// PkiReconciler is part of the reconciliation of redpanda.vectorized.io CRD.
// It creates certificates for Redpanda and its clients when TLS is enabled.
type PkiReconciler struct {
	k8sclient.Client
	scheme       *runtime.Scheme
	pandaCluster *redpandav1alpha1.Cluster
	internalFQDN string
	logger       logr.Logger
}

// NewPki creates PkiReconciler
func NewPki(
	client k8sclient.Client,
	pandaCluster *redpandav1alpha1.Cluster,
	fqdn string,
	scheme *runtime.Scheme,
	logger logr.Logger,
) *PkiReconciler {
	return &PkiReconciler{
		client, scheme, pandaCluster, fqdn, logger.WithValues("Reconciler", "pki"),
	}
}

func (r *PkiReconciler) prepareRoot() (
	[]resources.Resource,
	*cmmetav1.ObjectReference,
) {
	toApply := []resources.Resource{}

	selfSignedKey := r.issuerNamespacedName("selfsigned-issuer")
	selfSignedIssuer := NewIssuer(r.Client,
		r.scheme,
		r.pandaCluster,
		selfSignedKey,
		"",
		r.logger)

	rootCn := NewCommonName(r.pandaCluster.Name, "root-certificate")
	rootKey := types.NamespacedName{Name: string(rootCn), Namespace: r.pandaCluster.Namespace}
	rootCertificate := NewCertificate(r.Client,
		r.scheme,
		r.pandaCluster,
		rootKey,
		selfSignedIssuer.objRef(),
		rootCn,
		true,
		r.logger)

	k8sClusterIssuerKey := r.issuerNamespacedName("root-issuer")
	k8sClusterIssuer := NewIssuer(r.Client,
		r.scheme,
		r.pandaCluster,
		k8sClusterIssuerKey,
		rootCertificate.Key().Name,
		r.logger)

	selfSignedIssuerRef := k8sClusterIssuer.objRef()

	toApply = append(toApply, selfSignedIssuer, rootCertificate, k8sClusterIssuer)
	return toApply, selfSignedIssuerRef
}

// Ensure will manage PKI for redpanda.vectorized.io custom resource
func (r *PkiReconciler) Ensure(ctx context.Context) error {
	if !r.pandaCluster.Spec.Configuration.TLS.KafkaAPI.Enabled &&
		!r.pandaCluster.Spec.Configuration.TLS.AdminAPI.Enabled {
		return nil
	}

	toApply, selfSignedIssuerRef := r.prepareRoot()

	if r.pandaCluster.Spec.Configuration.TLS.KafkaAPI.Enabled {
		applyKafka, err := r.prepareKafkaAPI(ctx, selfSignedIssuerRef)
		if err != nil {
			return err
		}
		toApply = append(toApply, applyKafka...)
	}

	if r.pandaCluster.Spec.Configuration.TLS.AdminAPI.Enabled {
		toApply = append(toApply, r.prepareAdminAPI(selfSignedIssuerRef)...)
	}

	for _, res := range toApply {
		err := res.Ensure(ctx)
		if err != nil {
			r.logger.Error(err, "Failed to reconcile pki")
		}
	}

	return nil
}

func (r *PkiReconciler) issuerNamespacedName(name string) types.NamespacedName {
	return types.NamespacedName{Name: r.pandaCluster.Name + "-" + name, Namespace: r.pandaCluster.Namespace}
}
