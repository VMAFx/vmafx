// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// C ABI of the Rust predictor (contract section 3 and 8, lane P). The only
// module of `vmafx-predict` that contains `unsafe`. The generated header
// section lists `vmafx_rs_model_new`, `vmafx_rs_model_predict` and
// `vmafx_rs_model_free`; `VmafxRsModelView` is a flat, borrowed view that the
// C shim (`core/src/rust/shim/rust_predict.c`) fills from `struct VmafModel`.

use core::ffi::c_void;

use crate::model::{
    Chroma, Feature, Kernel, Model, ModelError, Norm, Point, Stage, SvNode, SvmView, TransformView,
};
use crate::predict::PredictError;

use vmafx_fex::abi::{
    VMAFX_RS_E_INVAL, VMAFX_RS_E_NOMEM, VMAFX_RS_E_NONFINITE, VMAFX_RS_E_NOTSUP, VMAFX_RS_OK,
};

/// `VmafModelNormalizationType`
const NORM_NONE: u32 = 1;
const NORM_LINEAR_RESCALE: u32 = 2;
/// `svm_parameter.svm_type` (`EPSILON_SVR`, `NU_SVR`)
const SVM_EPSILON_SVR: i32 = 3;
const SVM_NU_SVR: i32 = 4;
/// `svm_parameter.kernel_type`
const KERNEL_LINEAR: i32 = 0;
const KERNEL_POLY: i32 = 1;
const KERNEL_RBF: i32 = 2;

/// Opaque handle owned by Rust: `vmafx_rs_model_new` .. `vmafx_rs_model_free`.
pub struct VmafxRsModel {
    model: Model,
}

/// Flat view of a `VmafModel` and its `svm_model`. Every pointer is borrowed
/// for the duration of `vmafx_rs_model_new` only; Rust copies what it keeps.
/// Flags are 0 / 1.
#[repr(C)]
pub struct VmafxRsModelView {
    /// `VmafModelNormalizationType`
    pub norm_type: u32,
    pub slope: f64,
    pub intercept: f64,
    pub n_features: u32,
    pub feature_slope: *const f64,
    pub feature_intercept: *const f64,
    /// `strstr(name, "adm3") != NULL`, one byte per feature
    pub feature_guiding: *const u8,
    /// `strstr(name, "speed_chroma") != NULL`, one byte per feature
    pub feature_guided: *const u8,
    /// `chroma_from_luma.enabled && chroma_correction_parameter != 0`
    pub chroma_enabled: u32,
    pub chroma_parameter: f64,
    pub transform_enabled: u32,
    pub p0_enabled: u32,
    pub p0: f64,
    pub p1_enabled: u32,
    pub p1: f64,
    pub p2_enabled: u32,
    pub p2: f64,
    pub knots_enabled: u32,
    pub n_knots: u32,
    pub knot_x: *const f64,
    pub knot_y: *const f64,
    pub out_lte_in: u32,
    pub out_gte_in: u32,
    pub clip_enabled: u32,
    pub clip_min: f64,
    pub clip_max: f64,
    /// `svm_parameter.svm_type`
    pub svm_type: i32,
    /// `svm_parameter.kernel_type`
    pub kernel_type: i32,
    pub degree: i32,
    pub gamma: f64,
    pub coef0: f64,
    /// `model->l`
    pub n_sv: u32,
    /// `model->sv_coef[0]`, `n_sv` values
    pub sv_coef: *const f64,
    /// `model->rho[0]`
    pub rho: f64,
    /// `n_sv + 1` offsets into the node arrays
    pub sv_start: *const u32,
    pub n_nodes: u32,
    pub sv_index: *const i32,
    pub sv_value: *const f64,
}

/// Failure detail of `vmafx_rs_model_predict` (status `VMAFX_RS_E_NONFINITE`).
#[repr(C)]
pub struct VmafxRsPredictFail {
    /// 0 model score, 1 score transform, 2 piecewise score
    pub stage: u32,
    pub value: f64,
}

/// Borrowed array of a view; a NULL pointer is valid for an empty array only.
///
/// # Safety
/// `ptr` is valid for reads of `n` values of `T` for `'a`, or `n == 0`.
unsafe fn array<'a, T>(ptr: *const T, n: usize) -> Result<&'a [T], i32> {
    if n == 0 {
        return Ok(&[]);
    }
    if ptr.is_null() || !ptr.is_aligned() {
        return Err(VMAFX_RS_E_INVAL);
    }
    // SAFETY: non-null, aligned (checked above); the caller guarantees `n`
    // readable values for `'a`.
    Ok(unsafe { std::slice::from_raw_parts(ptr, n) })
}

fn reserve<T: Clone>(src: &[T]) -> Result<Vec<T>, ModelError> {
    let mut v = Vec::new();
    v.try_reserve_exact(src.len())?;
    v.extend_from_slice(src);
    Ok(v)
}

fn flag(v: u32) -> bool {
    v != 0
}

fn opt(enabled: u32, value: f64) -> Option<f64> {
    flag(enabled).then_some(value)
}

fn norm_of(v: u32) -> Result<Norm, ModelError> {
    match v {
        NORM_NONE => Ok(Norm::None),
        NORM_LINEAR_RESCALE => Ok(Norm::LinearRescale),
        _ => Err(ModelError::Unsupported("normalization type")),
    }
}

fn kernel_of(v: &VmafxRsModelView) -> Result<Kernel, ModelError> {
    match v.kernel_type {
        KERNEL_LINEAR => Ok(Kernel::Linear),
        KERNEL_POLY => Ok(Kernel::Poly {
            gamma: v.gamma,
            coef0: v.coef0,
            degree: v.degree,
        }),
        KERNEL_RBF => Ok(Kernel::Rbf { gamma: v.gamma }),
        _ => Err(ModelError::Unsupported("svm kernel type")),
    }
}

/// # Safety
/// The view's pointers satisfy the contract of [`array`] for their lengths.
unsafe fn features_of(v: &VmafxRsModelView) -> Result<Vec<Feature>, i32> {
    let n = v.n_features as usize;
    // SAFETY: forwarded from the caller's guarantee on the view.
    let (slope, icpt, guiding, guided) = unsafe {
        (
            array(v.feature_slope, n)?,
            array(v.feature_intercept, n)?,
            array(v.feature_guiding, n)?,
            array(v.feature_guided, n)?,
        )
    };
    let mut out = Vec::new();
    out.try_reserve_exact(n).map_err(|_| VMAFX_RS_E_NOMEM)?;
    for i in 0..n {
        out.push(Feature {
            slope: slope[i],
            intercept: icpt[i],
            guiding: guiding[i] != 0,
            guided: guided[i] != 0,
        });
    }
    Ok(out)
}

/// # Safety
/// The view's knot pointers satisfy the contract of [`array`].
unsafe fn transform_of(v: &VmafxRsModelView) -> Result<Option<TransformView>, i32> {
    if !flag(v.transform_enabled) {
        return Ok(None);
    }
    let knots = if flag(v.knots_enabled) {
        let n = v.n_knots as usize;
        // SAFETY: forwarded from the caller's guarantee on the view.
        let (x, y) = unsafe { (array(v.knot_x, n)?, array(v.knot_y, n)?) };
        let mut list = Vec::new();
        list.try_reserve_exact(n).map_err(|_| VMAFX_RS_E_NOMEM)?;
        list.extend(x.iter().zip(y).map(|(x, y)| Point { x: *x, y: *y }));
        Some(list)
    } else {
        None
    };
    Ok(Some(TransformView {
        p0: opt(v.p0_enabled, v.p0),
        p1: opt(v.p1_enabled, v.p1),
        p2: opt(v.p2_enabled, v.p2),
        knots,
        out_lte_in: flag(v.out_lte_in),
        out_gte_in: flag(v.out_gte_in),
    }))
}

/// # Safety
/// The view's support vector pointers satisfy the contract of [`array`].
unsafe fn svm_of(v: &VmafxRsModelView) -> Result<SvmView, i32> {
    if v.svm_type != SVM_EPSILON_SVR && v.svm_type != SVM_NU_SVR {
        return Err(VMAFX_RS_E_NOTSUP);
    }
    let l = v.n_sv as usize;
    let nn = v.n_nodes as usize;
    // SAFETY: forwarded from the caller's guarantee on the view.
    let (coef, start, index, value) = unsafe {
        (
            array(v.sv_coef, l)?,
            array(v.sv_start, l + 1)?,
            array(v.sv_index, nn)?,
            array(v.sv_value, nn)?,
        )
    };
    let kernel = kernel_of(v).map_err(status_of_model)?;
    let mut nodes = Vec::new();
    nodes.try_reserve_exact(nn).map_err(|_| VMAFX_RS_E_NOMEM)?;
    nodes.extend(index.iter().zip(value).map(|(i, x)| SvNode {
        index: *i,
        value: *x,
    }));
    Ok(SvmView {
        kernel,
        rho: v.rho,
        coef: reserve(coef).map_err(status_of_model)?,
        start: offsets(start)?,
        nodes,
    })
}

fn offsets(start: &[u32]) -> Result<Vec<usize>, i32> {
    let mut out = Vec::new();
    out.try_reserve_exact(start.len())
        .map_err(|_| VMAFX_RS_E_NOMEM)?;
    out.extend(start.iter().map(|s| *s as usize));
    Ok(out)
}

fn status_of_model(e: ModelError) -> i32 {
    match e {
        ModelError::InvalidArgument(_) => VMAFX_RS_E_INVAL,
        ModelError::Unsupported(_) => VMAFX_RS_E_NOTSUP,
        ModelError::OutOfMemory => VMAFX_RS_E_NOMEM,
    }
}

/// # Safety
/// The view's pointers satisfy the contract of [`array`].
unsafe fn model_of(v: &VmafxRsModelView) -> Result<Model, i32> {
    let norm = norm_of(v.norm_type).map_err(status_of_model)?;
    let chroma = flag(v.chroma_enabled).then_some(Chroma {
        parameter: v.chroma_parameter,
    });
    let clip = flag(v.clip_enabled).then_some((v.clip_min, v.clip_max));
    // SAFETY: forwarded from the caller's guarantee on the view.
    let (features, transform, svm) = unsafe { (features_of(v)?, transform_of(v)?, svm_of(v)?) };
    Model::new(
        norm,
        v.slope,
        v.intercept,
        features,
        chroma,
        transform,
        clip,
        svm,
    )
    .map_err(status_of_model)
}

/// Builds the predictor from `view`; `*out` receives the handle.
///
/// Returns `VMAFX_RS_OK`, `VMAFX_RS_E_NOTSUP` (a model the Rust predictor does
/// not implement: the caller runs the C predictor and says so),
/// `VMAFX_RS_E_INVAL` or `VMAFX_RS_E_NOMEM`.
///
/// # Safety
/// `view` points to a `VmafxRsModelView` whose pointers are valid for reads of
/// the lengths its counts give, for the duration of the call; `out` is valid
/// for one pointer write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmafx_rs_model_new(
    view: *const VmafxRsModelView,
    out: *mut *mut VmafxRsModel,
) -> i32 {
    if view.is_null() || out.is_null() {
        return VMAFX_RS_E_INVAL;
    }
    // SAFETY: `view` is non-null; the caller guarantees it is a valid view.
    let view = unsafe { &*view };
    // SAFETY: the caller guarantees the view's arrays.
    let model = match unsafe { model_of(view) } {
        Ok(m) => m,
        Err(status) => return status,
    };
    let handle = Box::into_raw(Box::new(VmafxRsModel { model }));
    // SAFETY: `out` is non-null and valid for one write (caller guarantee).
    unsafe { *out = handle };
    VMAFX_RS_OK
}

fn stage_code(s: Stage) -> u32 {
    match s {
        Stage::ModelScore => 0,
        Stage::ScoreTransform => 1,
        Stage::PiecewiseScore => 2,
    }
}

/// Predicts one frame from the raw feature scores in model order.
///
/// `nodes` is caller scratch of `n_nodes >= n_features + 1` entries laid out as
/// `struct svm_node` (`{ int index; double value; }`, 16 bytes); the predictor does not
/// allocate. `fail` may be NULL; on `VMAF_RS_E_NONFINITE` it receives the stage
/// and the offending value.
///
/// # Safety
/// `model` came from `vmafx_rs_model_new` and is not freed during the call;
/// `scores` is valid for `n_scores` reads, `nodes` for `n_nodes` reads and
/// writes with no other thread using it, `out` for one write, `fail` NULL or
/// valid for one write.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmafx_rs_model_predict(
    model: *const VmafxRsModel,
    scores: *const f64,
    n_scores: usize,
    flags: u32,
    nodes: *mut c_void,
    n_nodes: usize,
    out: *mut f64,
    fail: *mut VmafxRsPredictFail,
) -> i32 {
    let nodes = nodes.cast::<SvNode>();
    if model.is_null() || scores.is_null() || nodes.is_null() || out.is_null() {
        return VMAFX_RS_E_INVAL;
    }
    if !scores.is_aligned() || !nodes.is_aligned() {
        return VMAFX_RS_E_INVAL;
    }
    // SAFETY: non-null, valid for the stated lengths by the caller's contract.
    let (model, scores, nodes) = unsafe {
        (
            &(*model).model,
            std::slice::from_raw_parts(scores, n_scores),
            { std::slice::from_raw_parts_mut(nodes, n_nodes) },
        )
    };
    match model.predict(scores, flags, nodes) {
        Ok(v) => {
            // SAFETY: `out` is non-null and valid for one write.
            unsafe { *out = v };
            VMAFX_RS_OK
        }
        Err(e) => {
            // SAFETY: `fail` is NULL or valid for one write.
            unsafe { report(e, fail) }
        }
    }
}

/// # Safety
/// `fail` is NULL or valid for one write.
unsafe fn report(e: PredictError, fail: *mut VmafxRsPredictFail) -> i32 {
    match e {
        PredictError::Invalid => VMAFX_RS_E_INVAL,
        PredictError::NonFinite { stage, value } => {
            if !fail.is_null() {
                // SAFETY: non-null, valid for one write (caller guarantee).
                unsafe {
                    *fail = VmafxRsPredictFail {
                        stage: stage_code(stage),
                        value,
                    }
                };
            }
            VMAFX_RS_E_NONFINITE
        }
    }
}

/// Frees a handle from `vmafx_rs_model_new`; NULL is a no-op.
///
/// # Safety
/// `model` is NULL or a handle not yet freed, and no call uses it afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn vmafx_rs_model_free(model: *mut VmafxRsModel) {
    if !model.is_null() {
        // SAFETY: the handle came from `Box::into_raw` and is freed once.
        drop(unsafe { Box::from_raw(model) });
    }
}
