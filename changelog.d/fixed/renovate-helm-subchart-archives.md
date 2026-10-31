- **Renovate: update packaged Helm subchart archives on Chart.lock changes.**
  Configures top-level `postUpdateOptions: ["helmUpdateSubChartArchives"]` in
  `renovate.json` so subchart archive tarballs (`deploy/helm/.../charts/*.tgz`)
  are refreshed when Renovate updates `Chart.lock` (#3045, #3047), preventing
  `helm lint` failures caused by absent subchart packages.
