// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2

use vmafx::Status;
use vmafx::api_gen::{
    ABI_VERSION, ABI_VERSION_MAJOR, ABI_VERSION_MINOR, ABI_VERSION_PATCH, Backend, Context,
    ContextConfig, Error, Pool, abi_version, backend_name, model_builtin_next,
    model_default_version, status_name, version_string,
};

const ENOENT: i32 = 2;

#[test]
fn library_functions() {
    assert!(!version_string().is_empty());
    let (major, minor, patch) = abi_version();
    assert_eq!(
        (major, minor, patch),
        (ABI_VERSION_MAJOR, ABI_VERSION_MINOR, ABI_VERSION_PATCH)
    );
    assert_eq!((major, minor, patch), ABI_VERSION);
    assert_eq!(status_name(Status::EInvalid), "VMAFX_E_INVALID");
    assert!(!model_default_version().is_empty());
    assert!(!model_builtin_next("").is_empty());
    assert_eq!(backend_name(Backend::CPU), "cpu");
}

#[test]
fn context_import_and_pooled_score() -> Result<(), Error> {
    let cfg = ContextConfig {
        n_threads: 1,
        ..ContextConfig::default()
    };
    let ctx = Context::new(Some(&cfg))?;
    assert!(!ctx.as_raw().is_null());
    assert!(!ctx.libvmaf_handle().is_null());

    let prov = ctx.provenance()?;
    assert_eq!(prov.abi_major, ABI_VERSION_MAJOR);
    assert_eq!(prov.n_threads, 1);

    ctx.import_score("x", 0, 1.0)?;
    ctx.import_score("x", 1, 2.0)?;

    let score = ctx.feature_score("x", 0)?;
    assert!((score.value - 1.0).abs() < f64::EPSILON);

    let pooled = ctx.feature_score_pooled("x", Pool::MEAN, 0, 1)?;
    assert!((pooled.value - 1.5).abs() < f64::EPSILON);
    assert_eq!(pooled.first, 0);
    assert_eq!(pooled.last, 1);
    assert_eq!(pooled.pool, Pool::MEAN);

    ctx.close()?;
    Ok(())
}

#[test]
fn context_negative_and_close_contract() -> Result<(), Box<dyn std::error::Error>> {
    let bad_cfg = ContextConfig {
        import_retry_wait_ns: 600_000_000_001,
        ..ContextConfig::default()
    };
    let Err(cfg_err) = Context::new(Some(&bad_cfg)) else {
        return Err("expected Context::new with out-of-range wait to fail".into());
    };
    assert_eq!(cfg_err.status, Status::ERange);
    assert_eq!(cfg_err.subject, "config.import_retry_wait_ns");

    let ctx = Context::new(None)?;
    let Err(ext_err) = ctx.extractor_info(99_999) else {
        return Err("expected extractor_info(99_999) to fail".into());
    };
    assert_eq!(ext_err.status, Status::ENotFound);
    assert_eq!(ext_err.subject, "index");
    assert_eq!(ext_err.errno, -ENOENT);
    assert!(!ext_err.is_pending());
    assert!(ext_err.to_string().contains("vmafx_context_extractor_info"));

    // ADR-1336 retry-safe close contract: a failed close hands the live Context back
    // so the caller can still inspect or retry closing it; once close() succeeds,
    // Drop runs on the consumed Context without a second destroy call.
    let Err((ctx, close_err)) = ctx.close_with(|_| {
        Err(Error {
            function: "vmafx_context_destroy".to_owned(),
            status: Status::EBusy,
            message: "simulated transient teardown failure".to_owned(),
            subject: String::new(),
            errno: -16,
        })
    }) else {
        return Err("expected simulated close failure to return (Context, Error)".into());
    };
    assert_eq!(close_err.status, Status::EBusy);
    assert!(!ctx.as_raw().is_null());
    assert_eq!(ctx.provenance()?.abi_major, ABI_VERSION_MAJOR);

    ctx.close().map_err(|(_, err)| err)?;
    Ok(())
}

#[test]
fn context_pending_boundary() -> Result<(), Box<dyn std::error::Error>> {
    let ctx = Context::new(None)?;
    ctx.import_score("x", 0, 1.0)?;

    let Err(pending_err) = ctx.feature_score_pooled("x", Pool::MEAN, 0, 1) else {
        return Err("expected feature_score_pooled on unscored frame 1 to return Pending".into());
    };
    assert_eq!(pending_err.status, Status::Pending);
    assert!(pending_err.is_pending());

    ctx.import_score("x", 1, 2.0)?;
    let pooled = ctx.feature_score_pooled("x", Pool::MEAN, 0, 1)?;
    assert!((pooled.value - 1.5).abs() < f64::EPSILON);

    ctx.close().map_err(|(_, err)| err)?;
    Ok(())
}
