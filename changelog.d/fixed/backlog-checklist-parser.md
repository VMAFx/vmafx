- Fixed the agent eligibility tracker parsing zero current backlog items after
  the ledger moved from a pipe table to Markdown checklists. Checklist items now
  carry stable IDs, schema drift blocks dispatch, and legacy rows remain
  readable during migration.
