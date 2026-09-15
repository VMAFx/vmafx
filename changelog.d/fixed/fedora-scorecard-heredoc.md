- Fix the Fedora development Dockerfile's optional SYCL repository setup so
  Docker and Scorecard can parse the file even when SYCL is disabled; retain
  the existing signature checks and package-install failure propagation.
