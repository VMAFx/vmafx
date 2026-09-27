- `dev-container-publish.yml` finishes and signs the dev container image again.
  Six of its last ten master runs were cancelled at the 60-minute timeout while
  exporting a layer cache that cannot fit GitHub's 10 GB cache limit, after the
  image was pushed but before cosign signed it. The cache export is gone and the
  timeout is 90 minutes.
