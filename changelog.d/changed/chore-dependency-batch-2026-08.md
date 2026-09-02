- Two months of accumulated dependency updates land as a single batch instead of
  36 individual PRs: Docker base-image digests and tags (debian, fedora 45→46,
  golang 1.26→1.27, ubuntu, python-slim, both distroless variants,
  docker/dockerfile, nvidia/cuda), Go module minor/patch updates, GitHub Actions
  minor/patch, the Helm prometheus-pushgateway chart, and the Python dependency
  floors (numpy, pandas, scipy, matplotlib, torch, torchvision, transformers,
  onnxruntime, onnxscript, pyarrow, pydantic, ray, tqdm, typer, mypy, ruff,
  types-pyyaml, anthropic, openai, mcp). Batching was necessary because the CI
  queue could not drain 45 concurrent dependency PRs inside the merge gate's
  deadline — see ADR-1123 for the aggregator half of that fix.
