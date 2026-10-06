- The Grafana overview dashboard (`deploy/grafana/vmafx-overview.json`) now
  queries only series the binaries register: jobs queued, jobs in flight and
  active nodes read `vmafx_controller_jobs_pending`,
  `vmafx_controller_jobs_running` and `vmafx_controller_nodes_live`. The frame
  throughput and GPU utilisation panels are removed because no binary records
  those instruments yet. `TestDashboardQueriesOnlyRegisteredMetrics` fails when
  a panel names an unregistered series (#1251).
