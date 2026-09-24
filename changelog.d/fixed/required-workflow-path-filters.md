- Fixed twelve required checks across seven workflows that could silently disappear
  behind incomplete trigger path filters. The workflows now always start, route
  expensive work through the shared impact planner, and always emit exact-name
  fail-closed gates; the aggregator follows their delayed job dependencies across
  every Checks API page, and the regression guard now detects nested YAML filters.
