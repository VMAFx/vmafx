# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
# Planted defect for the Trivy image gate: 18.04 is end-of-life and carries
# known HIGH/CRITICAL operating-system advisories. The docker-image workflow
# builds this image, requires `trivy image` to reject it, and only then scans
# the real image. Never use this as a runtime base.
FROM ubuntu:18.04
