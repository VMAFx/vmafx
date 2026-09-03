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
const DefaultVersion = "vmaf_v1.0.16_3d0h"

// DefaultNEGVersion is the model used when a caller asks for NEG scoring
// without naming a model.
//
// It is deliberately NOT derived from DefaultVersion. No-Enhancement-Gain is a
// v0.6.1-family concept: Netflix published NEG variants for vmaf_v0.6.1,
// vmaf_float_v0.6.1 and vmaf_4k_v0.6.1 only, and there is no NEG counterpart to
// any vmaf_v1.0.16_* model. Appending "neg" to DefaultVersion would synthesise
// "vmaf_v1.0.16_3d0hneg", which does not exist and which libvmaf would reject
// at load time. Until Netflix publishes a v1 NEG model, NEG scoring stays on
// the v0.6.1 family and the two constants intentionally name different
// generations.
const DefaultNEGVersion = "vmaf_v0.6.1neg"
