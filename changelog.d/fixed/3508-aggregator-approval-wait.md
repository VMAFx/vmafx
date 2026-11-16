- **The required-checks aggregator now names edited-event Rules runs that await fork approval.** When the
  head SHA has an unstamped `rule-enforcement.yml` run with `action_required` or `waiting`, the aggregator
  still fails closed, but reports the run id and says to approve or re-run it instead of claiming the Rules
  workflow did not start; runs that truly never started keep the existing message.
