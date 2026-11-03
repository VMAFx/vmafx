// Copyright 2026 Lusoris
// SPDX-License-Identifier: EUPL-1.2
//
// tests/netflix_bitexact.rs — bit-exact parity with C API vmafx_score_contract (#2434).

use std::env;
use std::io::Read;
use std::path::{Path, PathBuf};

const WIDTH: u32 = 576;
const HEIGHT: u32 = 324;
const MAX_FRAMES: u32 = 4096;

unsafe extern "C" {
    fn strtod(nptr: *const std::ffi::c_char, endptr: *mut *mut std::ffi::c_char) -> f64;
    #[cfg_attr(target_os = "linux", link_name = "__errno_location")]
    #[cfg_attr(target_vendor = "apple", link_name = "__error")]
    #[cfg_attr(windows, link_name = "_errno")]
    fn errno_location() -> *mut std::ffi::c_int;
}

struct InputPaths {
    reference: PathBuf,
    distorted: PathBuf,
    model: PathBuf,
    contract_bin: PathBuf,
}

struct CScorePayload {
    vmaf: f64,
    vmaf_hex: String,
    frames: u32,
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

fn resolve_yuv_path(root: &Path, env_direct: &str, alt_env: &str, default_file: &str) -> PathBuf {
    if let Ok(v) = env::var(env_direct) {
        PathBuf::from(v)
    } else if let Ok(v) = env::var(alt_env) {
        PathBuf::from(v)
    } else if let Ok(dir) = env::var("VMAFX_YUV_DIR") {
        PathBuf::from(dir).join(default_file)
    } else if let Ok(dir) = env::var("VMAFX_CONTRACT_YUV") {
        PathBuf::from(dir).join(default_file)
    } else {
        root.join("python/test/resource/yuv").join(default_file)
    }
}

fn resolve_contract_bin(root: &Path) -> PathBuf {
    resolve_contract_candidates(
        root,
        env::var_os("VMAFX_CONTRACT_CAPI").as_deref().map(Path::new),
        env::var_os("VMAFX_LIBRARY").as_deref().map(Path::new),
        env::var_os("LIBVMAF_PREFIX").as_deref().map(Path::new),
    )
}

fn resolve_contract_candidates(
    root: &Path,
    explicit: Option<&Path>,
    library: Option<&Path>,
    prefix: Option<&Path>,
) -> PathBuf {
    let binary = format!("vmafx_score_contract{}", env::consts::EXE_SUFFIX);
    let inferred = library
        .and_then(Path::parent)
        .and_then(Path::parent)
        .map(|p| p.join("test").join(&binary));
    let installed = prefix.map(|p| p.join("test").join(&binary));
    let candidates = [
        explicit.map(Path::to_path_buf),
        inferred,
        installed,
        Some(root.join(".b/test").join(&binary)),
        Some(root.join("build/test").join(&binary)),
    ];
    candidates
        .iter()
        .flatten()
        .find(|p| p.is_file())
        .cloned()
        .unwrap_or_else(|| {
            explicit.map_or_else(|| root.join("build/test").join(binary), Path::to_path_buf)
        })
}

fn find_inputs() -> InputPaths {
    let root = repo_root();
    let reference = resolve_yuv_path(
        &root,
        "VMAFX_YUV_REF",
        "VMAFX_YUV_REF",
        "src01_hrc00_576x324.yuv",
    );
    let distorted = resolve_yuv_path(
        &root,
        "VMAFX_YUV_DIST",
        "VMAFX_YUV_DIS",
        "src01_hrc01_576x324.yuv",
    );
    let model = if let Ok(v) = env::var("VMAFX_MODEL") {
        PathBuf::from(v)
    } else if let Ok(v) = env::var("VMAFX_CONTRACT_MODEL_DIR") {
        PathBuf::from(v).join("vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json")
    } else {
        root.join("model/vmaf_v1.0.16/vmaf_v1.0.16_3d0h.json")
    };
    let contract_bin = resolve_contract_bin(&root);
    InputPaths {
        reference,
        distorted,
        model,
        contract_bin,
    }
}

fn check_inputs(
    paths: &InputPaths,
    require_pair: bool,
    env_required: Option<&str>,
) -> Result<bool, Box<dyn std::error::Error>> {
    let mut missing = Vec::new();
    if !paths.reference.is_file() {
        missing.push(format!("ref YUV ({})", paths.reference.display()));
    }
    if !paths.distorted.is_file() {
        missing.push(format!("dist YUV ({})", paths.distorted.display()));
    }
    if !paths.model.is_file() {
        missing.push(format!("model ({})", paths.model.display()));
    }
    if !paths.contract_bin.is_file() {
        missing.push(format!(
            "contract binary ({})",
            paths.contract_bin.display()
        ));
    }
    if missing.is_empty() {
        return Ok(true);
    }
    let is_required = require_pair || env_required.is_some_and(|v| !v.is_empty() && v != "0");
    if is_required {
        return Err(format!(
            "VMAFX_REQUIRE_NETFLIX_PAIR is set and inputs are missing: {}",
            missing.join(", ")
        )
        .into());
    }
    println!(
        "SKIP: missing inputs ({}); set VMAFX_YUV_DIR/VMAFX_CONTRACT_CAPI to run",
        missing.join(", ")
    );
    Ok(false)
}

fn parse_hex_float(s: &str) -> Result<f64, Box<dyn std::error::Error>> {
    // The contract emits C %a (hexadecimal), which Rust's decimal FromStr rejects.
    let c_str = std::ffi::CString::new(s)?;
    let mut end = std::ptr::null_mut();
    // SAFETY: CString owns a live NUL-terminated input; end is a writable out-pointer.
    // The platform C runtime supplies a non-null, thread-local errno pointer. Clear
    // and inspect it immediately around strtod; no other C call can overwrite it.
    // end is compared with the input's terminal NUL address, never dereferenced.
    let (val, error, consumed_all) = unsafe {
        let errno = errno_location();
        *errno = 0;
        let val = strtod(c_str.as_ptr(), &raw mut end);
        let error = *errno;
        (val, error, end.cast_const() == c_str.as_ptr().add(s.len()))
    };
    if s.is_empty() || !consumed_all || error != 0 || !val.is_finite() {
        return Err(format!("invalid C float {s:?} (errno={error})").into());
    }
    Ok(val)
}

fn extract_json_str(json: &str, key: &str) -> Result<String, Box<dyn std::error::Error>> {
    let pos = json.find(key).ok_or("key not found in contract JSON")?;
    let after = &json[pos.saturating_add(key.len())..];
    let first_quote = after.find('"').ok_or("opening quote not found")?;
    let rest = &after[first_quote.saturating_add(1)..];
    let end_quote = rest.find('"').ok_or("closing quote not found")?;
    Ok(rest[..end_quote].to_string())
}

fn extract_json_num(json: &str, key: &str) -> Result<f64, Box<dyn std::error::Error>> {
    let pos = json.find(key).ok_or("key not found in contract JSON")?;
    let after = json[pos.saturating_add(key.len())..].trim_start();
    let end = after
        .find(|c: char| c == ',' || c == '}' || c.is_whitespace())
        .unwrap_or(after.len());
    let val = after[..end].parse::<f64>()?;
    Ok(val)
}

fn parse_c_payload(json: &str) -> Result<CScorePayload, Box<dyn std::error::Error>> {
    let vmaf = extract_json_num(json, "\"vmaf\":")?;
    let vmaf_hex = extract_json_str(json, "\"vmaf_hex\":")?;
    #[allow(clippy::cast_possible_truncation, clippy::cast_sign_loss)]
    let frames = extract_json_num(json, "\"frames\":")? as u32;

    let hex_f64 = parse_hex_float(&vmaf_hex)?;
    if hex_f64.to_bits() != vmaf.to_bits() {
        return Err(
            format!("C contract internal mismatch: hex {vmaf_hex} != decimal {vmaf}").into(),
        );
    }
    Ok(CScorePayload {
        vmaf,
        vmaf_hex,
        frames,
    })
}

fn run_c_contract(paths: &InputPaths) -> Result<CScorePayload, Box<dyn std::error::Error>> {
    let ref_str = paths.reference.to_str().ok_or("invalid ref path")?;
    let dist_str = paths.distorted.to_str().ok_or("invalid dist path")?;
    let mut cmd = std::process::Command::new(&paths.contract_bin);
    cmd.args([
        ref_str,
        dist_str,
        "576",
        "324",
        "8",
        "vmaf_v1.0.16_3d0h",
        "0",
        "1",
    ]);
    if let Ok(ld) = env::var("LD_LIBRARY_PATH") {
        cmd.env("LD_LIBRARY_PATH", ld);
    }
    let output = cmd.output()?;
    if !output.status.success() {
        return Err(format!(
            "vmafx_score_contract failed (status={}): {}",
            output.status,
            String::from_utf8_lossy(&output.stderr)
        )
        .into());
    }
    let stdout = String::from_utf8(output.stdout)?;
    parse_c_payload(&stdout)
}

struct AllocatedPicture {
    picture: vmafx::Picture,
    capacities: [usize; 3],
}

impl AllocatedPicture {
    fn new() -> Result<Self, Box<dyn std::error::Error>> {
        let picture = vmafx::Picture::new_yuv420p_8bit(WIDTH, HEIGHT)?;
        let raw = picture.as_raw();
        let mut capacities = [0; 3];
        for (plane, capacity) in capacities.iter_mut().enumerate() {
            // Capture the allocation bounds before any raw metadata can be edited.
            // picture.h guarantees stride * height bytes per allocated plane.
            *capacity = usize::try_from(raw.stride[plane])?
                .checked_mul(usize::try_from(raw.h[plane])?)
                .ok_or("picture allocation extent overflow")?;
        }
        Ok(Self {
            picture,
            capacities,
        })
    }
}

struct PlaneLayout {
    data: *mut u8,
    width: usize,
    height: usize,
    stride: isize,
    capacity: usize,
}

fn validate_plane(
    layout: &PlaneLayout,
    width: usize,
    height: usize,
    source_len: usize,
) -> std::io::Result<usize> {
    let invalid = || std::io::Error::new(std::io::ErrorKind::InvalidInput, "invalid picture plane");
    let stride = usize::try_from(layout.stride).map_err(|_| invalid())?;
    if layout.data.is_null()
        || width == 0
        || height == 0
        || layout.width != width
        || layout.height != height
        || stride < width
        || width.checked_mul(height) != Some(source_len)
    {
        return Err(invalid());
    }
    let extent = (height - 1)
        .checked_mul(stride)
        .and_then(|v| v.checked_add(width))
        .ok_or_else(invalid)?;
    if extent > layout.capacity || extent > isize::MAX as usize {
        return Err(invalid());
    }
    Ok(stride)
}

fn copy_frame(pic: &mut AllocatedPicture, planes: &[Vec<u8>; 3]) -> std::io::Result<()> {
    let raw = pic.picture.as_raw_mut();
    for (plane, source) in planes.iter().enumerate() {
        let width = if plane == 0 { WIDTH } else { WIDTH / 2 } as usize;
        let height = if plane == 0 { HEIGHT } else { HEIGHT / 2 } as usize;
        let layout = PlaneLayout {
            data: raw.data[plane].cast(),
            width: raw.w[plane] as usize,
            height: raw.h[plane] as usize,
            stride: raw.stride[plane],
            capacity: pic.capacities[plane],
        };
        let stride = validate_plane(&layout, width, height, source.len())?;
        // SAFETY: AllocatedPicture retains exclusive ownership of the live C allocation
        // and the capacities captured at allocation. validate_plane checks non-null data,
        // positive matching dimensions, stride >= row bytes and the last row's extent
        // within that capacity and isize::MAX. Each bounded row stays inside it. The
        // exact-size source Vec is a separate live allocation, so copies cannot overlap.
        unsafe {
            for row in 0..height {
                std::ptr::copy_nonoverlapping(
                    source.as_ptr().add(row * width),
                    layout.data.add(row * stride),
                    width,
                );
            }
        }
    }
    Ok(())
}

fn read_plane(
    reader: &mut impl Read,
    bytes: &mut [u8],
    path: &Path,
    frame: u32,
    plane: usize,
) -> std::io::Result<()> {
    reader.read_exact(bytes).map_err(|err| {
        std::io::Error::new(
            err.kind(),
            format!("{}: frame {frame}, plane {plane}: {err}", path.display()),
        )
    })
}

fn read_frame_planes(
    reader: &mut impl Read,
    path: &Path,
    frame: u32,
) -> std::io::Result<Option<[Vec<u8>; 3]>> {
    let luma_bytes = WIDTH as usize * HEIGHT as usize;
    let mut planes = [
        vec![0; luma_bytes],
        vec![0; luma_bytes / 4],
        vec![0; luma_bytes / 4],
    ];
    // A one-byte read_exact distinguishes an empty next frame from a truncated one.
    match read_plane(reader, &mut planes[0][..1], path, frame, 0) {
        Err(err) if err.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        result => result?,
    }
    read_plane(reader, &mut planes[0][1..], path, frame, 0)?;
    for (plane, bytes) in planes.iter_mut().enumerate().skip(1) {
        read_plane(reader, bytes, path, frame, plane)?;
    }
    Ok(Some(planes))
}

fn read_yuv_frame(
    file: &mut std::fs::File,
    pic: &mut AllocatedPicture,
    path: &Path,
    frame: u32,
) -> std::io::Result<bool> {
    let Some(planes) = read_frame_planes(file, path, frame)? else {
        return Ok(false);
    };
    copy_frame(pic, &planes)?;
    Ok(true)
}

fn score_pair_rust(paths: &InputPaths) -> Result<(f64, u32), Box<dyn std::error::Error>> {
    let model = vmafx::Model::from_path(&paths.model)?;
    let mut ctx = vmafx::ContextBuilder::new()
        .n_threads(0)
        .n_subsample(1)
        .build()?;
    ctx.use_features_from_model(&model)?;

    let mut ref_file = std::fs::File::open(&paths.reference)?;
    let mut dist_file = std::fs::File::open(&paths.distorted)?;

    let mut n_frames = 0u32;
    for idx in 0..MAX_FRAMES {
        let mut r_pic = AllocatedPicture::new()?;
        let mut d_pic = AllocatedPicture::new()?;

        let r_ok = read_yuv_frame(&mut ref_file, &mut r_pic, &paths.reference, idx)?;
        let d_ok = read_yuv_frame(&mut dist_file, &mut d_pic, &paths.distorted, idx)?;
        if !r_ok || !d_ok {
            break;
        }
        ctx.read_pictures(r_pic.picture, d_pic.picture, idx)?;
        n_frames = idx.saturating_add(1);
    }
    if n_frames == 0 {
        return Err("zero frames read from fixtures".into());
    }
    ctx.flush()?;
    let pooled = ctx.score_pooled(
        &model,
        vmafx::PoolingMethod::Mean,
        0,
        n_frames.saturating_sub(1),
    )?;
    Ok((pooled, n_frames))
}

fn assert_bitexact_scores(got: f64, want: f64) -> Result<(), Box<dyn std::error::Error>> {
    let got_bits = got.to_bits();
    let want_bits = want.to_bits();
    if got_bits != want_bits {
        return Err(format!(
            "bit mismatch: got 0x{got_bits:016x} ({got}), want 0x{want_bits:016x} ({want})"
        )
        .into());
    }
    Ok(())
}

#[test]
fn test_bitexact_comparison_rejects_one_ulp_difference() -> Result<(), Box<dyn std::error::Error>> {
    let ref_val = 82.81606015944988_f64;
    let bits = ref_val.to_bits();
    let perturbed = f64::from_bits(bits.wrapping_add(1));
    let res = assert_bitexact_scores(perturbed, ref_val);
    if res.is_ok() {
        return Err("comparison helper must reject 1 ULP difference".into());
    }
    Ok(())
}

#[test]
fn test_missing_input_fails_when_pair_is_required() -> Result<(), Box<dyn std::error::Error>> {
    let absent_paths = InputPaths {
        reference: PathBuf::from("nonexistent_ref.yuv"),
        distorted: PathBuf::from("nonexistent_dist.yuv"),
        model: PathBuf::from("nonexistent_model.json"),
        contract_bin: PathBuf::from("nonexistent_contract"),
    };
    let res = check_inputs(&absent_paths, true, None);
    if res.is_ok() {
        return Err("check_inputs must fail when require_pair is true".into());
    }
    Ok(())
}

#[test]
fn test_netflix_576x324_bitexact_with_c_reference() -> Result<(), Box<dyn std::error::Error>> {
    let paths = find_inputs();
    let required = env::var("VMAFX_REQUIRE_NETFLIX_PAIR").ok();
    if !check_inputs(&paths, false, required.as_deref())? {
        return Ok(());
    }
    let c_ref = run_c_contract(&paths)?;
    let (rust_score, rust_frames) = score_pair_rust(&paths)?;

    if rust_frames != c_ref.frames {
        return Err(format!(
            "frame count mismatch: rust {} != c {}",
            rust_frames, c_ref.frames
        )
        .into());
    }
    assert!(!c_ref.vmaf_hex.is_empty());
    assert_bitexact_scores(rust_score, c_ref.vmaf)?;
    Ok(())
}

struct TestDir(PathBuf);

impl TestDir {
    fn new() -> std::io::Result<Self> {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let id = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let path = env::temp_dir().join(format!("vmafx-bitexact-{}-{id}", std::process::id()));
        std::fs::create_dir(&path)?;
        Ok(Self(path))
    }
}

impl Drop for TestDir {
    fn drop(&mut self) {
        if let Err(err) = std::fs::remove_dir_all(&self.0) {
            eprintln!("cannot remove test directory {}: {err}", self.0.display());
        }
    }
}

#[test]
fn test_hex_float_rejects_invalid_input() {
    for input in [
        "",
        "junk",
        "0x",
        "0x1p+",
        "0x1p0junk",
        "0x1p0 ",
        "1\0junk",
        "0x1p1024",
        "0x1p-99999",
        "nan",
        "NaN",
        "inf",
        "-infinity",
    ] {
        assert!(parse_hex_float(input).is_err(), "accepted {input:?}");
    }
}

#[test]
fn test_hex_float_preserves_bits() -> Result<(), Box<dyn std::error::Error>> {
    // Rust's FromStr cannot replace strtod for the C contract's %a format.
    assert!("0x1.4p6".parse::<f64>().is_err());
    for (input, expected) in [
        ("0x0p+0", 0.0_f64),
        ("-0x0p+0", -0.0_f64),
        ("0x1.4p6", 80.0_f64),
        ("0x1.0000000000001p0", f64::from_bits(1.0_f64.to_bits() + 1)),
        ("0x1p-1022", f64::MIN_POSITIVE),
        ("0x1.fffffffffffffp1023", f64::MAX),
    ] {
        assert_eq!(
            parse_hex_float(input)?.to_bits(),
            expected.to_bits(),
            "{input}"
        );
    }
    assert!(parse_hex_float("0x1p1024").is_err());
    assert_eq!(parse_hex_float("0x1p0")?.to_bits(), 1.0_f64.to_bits());
    Ok(())
}

#[test]
fn test_plane_layout_boundaries() {
    let mut buffer = [0_u8; 6];
    let valid = PlaneLayout {
        data: buffer.as_mut_ptr(),
        width: 2,
        height: 2,
        stride: 4,
        capacity: 6,
    };
    assert_eq!(validate_plane(&valid, 2, 2, 4).ok(), Some(4));
    let null = PlaneLayout {
        data: std::ptr::null_mut(),
        ..valid
    };
    assert!(validate_plane(&null, 2, 2, 4).is_err());
    for (width, height, stride, capacity) in [
        (2, 2, -4, 6),
        (2, 2, 1, 6),
        (2, 2, 4, 5),
        (0, 2, 4, 6),
        (2, 0, 4, 6),
        (3, 2, 4, 6),
        (2, 3, 4, 6),
        (2, 2, isize::MAX, 6),
    ] {
        let layout = PlaneLayout {
            width,
            height,
            stride,
            capacity,
            ..valid
        };
        assert!(validate_plane(&layout, 2, 2, 4).is_err());
    }
    assert!(validate_plane(&valid, 2, 2, 3).is_err());
    assert!(validate_plane(&valid, 2, 2, 5).is_err());
    assert!(validate_plane(&valid, 0, 2, 0).is_err());
    assert!(validate_plane(&valid, 2, 0, 0).is_err());
    let overflow = PlaneLayout {
        width: 1,
        height: usize::MAX,
        stride: isize::MAX,
        capacity: usize::MAX,
        ..valid
    };
    assert!(validate_plane(&overflow, 1, usize::MAX, usize::MAX).is_err());
    let too_large = PlaneLayout {
        stride: isize::MAX,
        capacity: usize::MAX,
        ..valid
    };
    assert!(validate_plane(&too_large, 2, 2, 4).is_err());
}

#[test]
fn test_truncated_frame_names_file_and_index() -> Result<(), Box<dyn std::error::Error>> {
    let dir = TestDir::new()?;
    let path = dir.0.join("truncated.yuv");
    let luma = WIDTH as usize * HEIGHT as usize;
    let frame_size = luma * 3 / 2;
    for length in [1, luma - 1, luma, luma + 1, luma + luma / 4, frame_size - 1] {
        std::fs::write(&path, vec![0; length])?;
        let mut file = std::fs::File::open(&path)?;
        let err = read_frame_planes(&mut file, &path, 7)
            .err()
            .ok_or("truncated frame accepted")?;
        assert_eq!(err.kind(), std::io::ErrorKind::UnexpectedEof);
        assert!(err.to_string().contains(&path.display().to_string()));
        assert!(err.to_string().contains("frame 7"));
    }
    Ok(())
}

#[test]
fn test_exact_frames_count_and_clean_eof() -> Result<(), Box<dyn std::error::Error>> {
    let dir = TestDir::new()?;
    let path = dir.0.join("exact.yuv");
    let frame_size = WIDTH as usize * HEIGHT as usize * 3 / 2;
    for count in [0, 1, 2] {
        std::fs::write(&path, vec![42; frame_size * count])?;
        let mut file = std::fs::File::open(&path)?;
        let mut actual = 0;
        for index in 0..=count {
            let frame = read_frame_planes(&mut file, &path, u32::try_from(index)?)?;
            if let Some(planes) = frame {
                assert!(planes.iter().flatten().all(|byte| *byte == 42));
                actual += 1;
            } else {
                break;
            }
        }
        assert_eq!(actual, count);
    }
    Ok(())
}

#[test]
fn test_short_reads_fill_each_plane() -> Result<(), Box<dyn std::error::Error>> {
    struct ShortReader(std::io::Cursor<Vec<u8>>);
    impl Read for ShortReader {
        fn read(&mut self, bytes: &mut [u8]) -> std::io::Result<usize> {
            let length = bytes.len().min(17);
            self.0.read(&mut bytes[..length])
        }
    }
    let size = WIDTH as usize * HEIGHT as usize * 3 / 2;
    let mut reader = ShortReader(std::io::Cursor::new(vec![42; size]));
    let planes =
        read_frame_planes(&mut reader, Path::new("short.yuv"), 0)?.ok_or("missing frame")?;
    assert_eq!(planes.iter().map(Vec::len).sum::<usize>(), size);
    assert!(planes.iter().flatten().all(|byte| *byte == 42));
    assert!(read_frame_planes(&mut reader, Path::new("short.yuv"), 1)?.is_none());
    Ok(())
}

#[test]
fn test_environment_requirement_rejects_missing_inputs() -> Result<(), Box<dyn std::error::Error>> {
    let dir = TestDir::new()?;
    let paths = InputPaths {
        reference: dir.0.join("ref"),
        distorted: dir.0.join("dist"),
        model: dir.0.join("model"),
        contract_bin: dir.0.join("contract"),
    };
    // Inject the env value at the single read boundary; no process-global env mutation.
    assert!(check_inputs(&paths, false, Some("1")).is_err());
    for value in [None, Some(""), Some("0")] {
        assert!(!check_inputs(&paths, false, value)?);
    }
    for path in [
        &paths.reference,
        &paths.distorted,
        &paths.model,
        &paths.contract_bin,
    ] {
        std::fs::write(path, [])?;
    }
    assert!(check_inputs(&paths, false, Some("1"))?);
    Ok(())
}

#[test]
fn test_contract_binary_falls_back_to_existing_candidates() -> Result<(), Box<dyn std::error::Error>>
{
    let dir = TestDir::new()?;
    let root = &dir.0;
    let prefix = root.join("installed");
    let library = prefix.join("lib/libvmafx.so");
    let binary = format!("vmafx_score_contract{}", env::consts::EXE_SUFFIX);
    let explicit = root.join("override");
    let installed = prefix.join("test").join(&binary);
    let local = root.join(".b/test").join(&binary);
    let build = root.join("build/test").join(&binary);
    assert!(!resolve_contract_candidates(root, None, Some(&library), Some(&prefix)).is_file());
    for candidate in [&build, &local, &installed, &explicit] {
        std::fs::create_dir_all(candidate.parent().ok_or("missing parent")?)?;
        std::fs::write(candidate, [])?;
        for (lib, pref) in [
            (Some(library.as_path()), None),
            (None, Some(prefix.as_path())),
            (Some(library.as_path()), Some(prefix.as_path())),
        ] {
            assert_eq!(
                resolve_contract_candidates(root, Some(&explicit), lib, pref),
                *candidate
            );
        }
    }
    Ok(())
}
