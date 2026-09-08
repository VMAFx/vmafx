Keep the existing SpEED registration and QA tests lint-clean through
read-only descriptor views and small temporal setup helpers. All assertions,
inputs and case registrations are preserved; helper failures still reach the
original test runner. The measured CPU warning baseline tightens by 26.
