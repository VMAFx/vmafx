// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// examples/score.rs — smoke test: score the Netflix golden pair through the safe API.
//
// Expected mean VMAF score (CPU, vmaf_v0.6.1): 76.669 (places=3)
// Full value: 76.66890519623612
//
// Usage:
//   VMAFX_REPO=<repo-root> cargo run --example score
//
// Or with explicit paths:
//   VMAFX_YUV_REF=<path> VMAFX_YUV_DIST=<path> VMAFX_MODEL=<path> \
//       cargo run --example score

use std::env;
use std::fs::File;
use std::io::Read;
use std::path::{Path, PathBuf};

use vmafx_sys::safe::{VmafContext, VmafModel, alloc_yuv420p_8bit, unref_picture, version};

const WIDTH: u32 = 576;
const HEIGHT: u32 = 324;
const N_FRAMES: u32 = 240;

struct InputPaths {
    reference: String,
    distorted: String,
    model: String,
}

fn repo_root() -> PathBuf {
    if let Ok(v) = env::var("VMAFX_REPO") {
        return PathBuf::from(v);
    }
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    manifest_dir
        .ancestors()
        .find(|dir| {
            (dir.join("meson.build").exists() || dir.join("Cargo.toml").exists())
                && dir.join("model").exists()
        })
        .map_or_else(|| PathBuf::from("."), Path::to_path_buf)
}

fn resolve_inputs(root: &Path) -> InputPaths {
    InputPaths {
        reference: env::var("VMAFX_YUV_REF").unwrap_or_else(|_| {
            root.join("python/test/resource/yuv/src01_hrc00_576x324.yuv")
                .to_string_lossy()
                .into_owned()
        }),
        distorted: env::var("VMAFX_YUV_DIST").unwrap_or_else(|_| {
            root.join("python/test/resource/yuv/src01_hrc01_576x324.yuv")
                .to_string_lossy()
                .into_owned()
        }),
        model: env::var("VMAFX_MODEL").unwrap_or_else(|_| {
            root.join("model/vmaf_v0.6.1.json")
                .to_string_lossy()
                .into_owned()
        }),
    }
}

fn inputs_available(paths: &InputPaths) -> bool {
    for (label, path) in [
        ("reference YUV", &paths.reference),
        ("distorted YUV", &paths.distorted),
        ("model", &paths.model),
    ] {
        if !Path::new(path).exists() {
            eprintln!("SKIP: {label} not found at {path} — test fixtures absent");
            return false;
        }
    }
    true
}

// similar_names: cb_plane/cr_plane and src_cb/src_cr are standard YUV
// chroma-plane naming; renaming would obscure the domain meaning.
#[allow(clippy::similar_names)]
fn read_yuv_frame(file: &mut File, pic: &mut vmafx_sys::VmafPicture) -> std::io::Result<bool> {
    let w = WIDTH as usize;
    let h = HEIGHT as usize;

    // luma
    let luma_bytes = w * h;
    let mut luma = vec![0u8; luma_bytes];
    let n = file.read(&mut luma)?;
    if n == 0 {
        return Ok(false); // EOF
    }
    if n < luma_bytes {
        return Ok(false);
    }

    // chroma (4:2:0)
    let chroma_w = w / 2;
    let chroma_h = h / 2;
    let chroma_bytes = chroma_w * chroma_h;
    let mut cb = vec![0u8; chroma_bytes];
    let mut cr = vec![0u8; chroma_bytes];
    file.read_exact(&mut cb)?;
    file.read_exact(&mut cr)?;

    // Copy into the VmafPicture planes.
    // cast_sign_loss: libvmaf strides are always non-negative for a valid picture.
    #[allow(clippy::cast_sign_loss)]
    // SAFETY: allocated picture planes cover the declared dimensions and strides.
    unsafe {
        let luma_plane = pic.data[0].cast::<u8>();
        let cb_plane = pic.data[1].cast::<u8>();
        let cr_plane = pic.data[2].cast::<u8>();

        let luma_stride = pic.stride[0] as usize;
        let cb_stride = pic.stride[1] as usize;
        let cr_stride = pic.stride[2] as usize;

        for row in 0..h {
            let src = &luma[(row * w)..((row + 1) * w)];
            let dst = luma_plane.add(row * luma_stride);
            std::ptr::copy_nonoverlapping(src.as_ptr(), dst, w);
        }
        for row in 0..chroma_h {
            let src_cb = &cb[(row * chroma_w)..((row + 1) * chroma_w)];
            let src_cr = &cr[(row * chroma_w)..((row + 1) * chroma_w)];
            let dst_cb = cb_plane.add(row * cb_stride);
            let dst_cr = cr_plane.add(row * cr_stride);
            std::ptr::copy_nonoverlapping(src_cb.as_ptr(), dst_cb, chroma_w);
            std::ptr::copy_nonoverlapping(src_cr.as_ptr(), dst_cr, chroma_w);
        }
    }
    Ok(true)
}

fn score_inputs(paths: &InputPaths) -> Result<(u32, f64), Box<dyn std::error::Error>> {
    let model = VmafModel::from_path(&paths.model)?;
    let mut ctx = VmafContext::new()?;
    ctx.use_features_from_model(&model)?;

    let mut ref_file = File::open(&paths.reference)
        .map_err(|e| format!("Cannot open reference YUV {}: {e}", paths.reference))?;
    let mut dist_file = File::open(&paths.distorted)
        .map_err(|e| format!("Cannot open distorted YUV {}: {e}", paths.distorted))?;

    let mut n_frames = 0u32;
    for frame_index in 0..N_FRAMES {
        let mut ref_pic = alloc_yuv420p_8bit(WIDTH, HEIGHT)?;
        let mut dist_pic = alloc_yuv420p_8bit(WIDTH, HEIGHT)?;

        // Round-3 R3-14: `VmafPicture` from the `-sys` layer has no `Drop`, so a
        // bare `?` on a read error would leak the plane buffers of both
        // already-allocated pictures. Capture the results and unref both
        // pictures on ANY error before propagating it (mirrors the clean-EOF
        // path below, which already unrefs).
        let read_result = read_yuv_frame(&mut ref_file, &mut ref_pic)
            .and_then(|ref_ok| read_yuv_frame(&mut dist_file, &mut dist_pic).map(|d| (ref_ok, d)));
        let (ref_ok, dist_ok) = match read_result {
            Ok(oks) => oks,
            Err(e) => {
                unref_picture(&mut ref_pic)?;
                unref_picture(&mut dist_pic)?;
                return Err(e.into());
            }
        };

        if !ref_ok || !dist_ok {
            unref_picture(&mut ref_pic)?;
            unref_picture(&mut dist_pic)?;
            break;
        }

        // Pass by move: ownership of both pictures transfers to libvmaf.
        ctx.read_pictures(ref_pic, dist_pic, frame_index)?;
        n_frames = frame_index + 1;
    }

    ctx.flush()?;
    if n_frames == 0 {
        eprintln!("ERROR: no frames read — check that the YUV paths are correct.");
        std::process::exit(1);
    }
    let score = ctx.score_pooled(&model, 0, n_frames - 1)?;
    ctx.close()
        .map_err(|err| std::io::Error::other(format!("vmaf context teardown failed: {err}")))?;
    Ok((n_frames, score))
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let paths = resolve_inputs(&repo_root());
    println!("vmafx-sys version: {}", version());
    println!("Reference:  {}", paths.reference);
    println!("Distorted:  {}", paths.distorted);
    println!("Model:      {}", paths.model);

    if !inputs_available(&paths) {
        return Ok(());
    }

    let (n_frames, score) = score_inputs(&paths)?;
    println!("Frames processed: {n_frames}");
    println!("Mean VMAF score:  {score:.4}");

    // Golden assertion: 76.669 places=3 (full value: 76.66890519623612)
    // Python golden gate for this sequence uses places=2; we assert places=3 here.
    let expected = 76.669_f64;
    let tolerance = 5e-3; // places=3
    if (score - expected).abs() > tolerance {
        eprintln!(
            "ASSERTION FAILED: expected {expected:.4}, got {score:.4} (delta={:.6})",
            (score - expected).abs()
        );
        std::process::exit(2);
    }
    println!("Score assertion PASSED (expected {expected:.4})");
    Ok(())
}
