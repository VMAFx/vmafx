- Fixed twelve required checks across seven workflows that could silently disappear
  behind incomplete trigger path filters. The workflows now always start, route
  expensive work through the shared impact planner, and always emit exact-name
  fail-closed gates; the regression guard now correctly detects nested YAML filters.
