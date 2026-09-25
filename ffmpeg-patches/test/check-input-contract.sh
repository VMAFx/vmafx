#!/bin/bash
set -euo pipefail

# Check that the 0001 patch contains the exact AV_LOG_INFO
if ! grep -F 'av_log(ctx, AV_LOG_INFO,' ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch >/dev/null; then
  echo "FAIL: 0001 patch missing AV_LOG_INFO"
  exit 1
fi
if ! grep -F 'libvmaf: input[0]=distorted input[1]=reference' ffmpeg-patches/0001-libvmaf-add-tiny-model-option.patch >/dev/null; then
  echo "FAIL: 0001 patch missing exact input order string"
  exit 1
fi

# Check that docs use the correct input order in examples
# Should not see "ffmpeg -i ref.* -i dis.*" or "ffmpeg -i reference.* -i distorted.*" in docs
BAD_MATCHES=$(grep -riE 'ffmpeg -i (ref|reference)\.[a-z0-9]+ -i (dis|distorted)\.[a-z0-9]+' docs/ ffmpeg-patches/ || true)
if [ -n "$BAD_MATCHES" ]; then
  echo "FAIL: Found reversed input order examples in docs:"
  echo "$BAD_MATCHES"
  exit 1
fi

echo "PASS: Input contract test"
