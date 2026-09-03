// Package model exposes the fork's single source of truth for which VMAF
// model is used when a caller names none.
//
// The authoritative definition is VMAF_DEFAULT_MODEL_VERSION in
// core/include/libvmaf/model.h. Anything that links libvmaf should call
// vmaf_default_model_version() and read it from the library directly. Most of
// this repository's Go binaries deliberately do not link libvmaf — forcing cgo
// on vmafx-tune, pkg/fast, pkg/bisect and pkg/corpus purely to learn a string
// would make them unbuildable without the C library — so they read the mirror
// below instead.
//
// The mirror is kept honest mechanically, not by discipline:
// scripts/ci/check-default-model-single-source.sh parses the C header and
// fails the build if this constant disagrees with it, and also fails if any
// component reintroduces its own hardcoded default. Changing the fork's
// default model is therefore a one-line edit to the C header plus whatever the
// gate then tells you to update.
package model

// DefaultVersion is the model version libvmaf scores with when no model is
// named. It must equal VMAF_DEFAULT_MODEL_VERSION in
// core/include/libvmaf/model.h; the CI gate above enforces that.
const DefaultVersion = "vmaf_v0.6.1"

// DefaultNEGVersion is the no-enhancement-gain companion to DefaultVersion,
// used where a caller asks for NEG scoring without naming a model.
const DefaultNEGVersion = "vmaf_v0.6.1neg"
