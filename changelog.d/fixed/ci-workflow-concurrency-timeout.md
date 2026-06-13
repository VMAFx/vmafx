Add concurrency guards to nightly, nightly-bisect, supply-chain, and release-please
workflows to prevent double-trigger races; add timeout-minutes to rust-ci (x2),
go-ci, release-please, and scorecard jobs to cap the 6-hour GitHub Actions default;
SHA-pin four mutable Docker/artifact action tags in e2e-k8s.yml (ADR-1035).
