---
paths:
  - core/src/feature/float_motion.c
  - core/src/feature/float_vif.c
invariant: Minimum-dimension guards cover every plane, not only luma.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Minimum-Dimension Guards Across All Picture Planes

## Minimum-dimension guards cover every plane, not just luma (ADR-1166)

`float_motion.c::motion_check_min_dim_all_planes` validates **chroma**
dimensions too when `motion_add_uv` is set, because `motion_blur_plane` is
called per plane with `ref_pic->w[c]` / `ref_pic->h[c]`. chroma geometry
must stay in step with `core/src/picture.c` (`(dim + ss) >> ss`); luma-only
guard is exactly bug Netflix/vmaf#1582 describes.
