# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Planted defect for the Trivy image gate: 18.04 is end-of-life and carries
# known HIGH/CRITICAL operating-system advisories. The docker-image workflow
# builds this image, requires `trivy image` to reject it, and only then scans
# the real image. Never use this as a runtime base.
FROM ubuntu:18.04@sha256:152dc042452c496007f07ca9127571cb9c29697f42acbfad72324b2bb2e43c98
# Non-root keeps this image's deliberate vulnerability separate from config lint.
USER 65534:65534
