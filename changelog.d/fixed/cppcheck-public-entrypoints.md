- Model 16 verified public C entrypoints in the shared local/CI Cppcheck
  configuration so missing external callers do not make disabled-backend APIs
  look unused. Keep private-function and body-defect checks, with declaration
  validation and real-tool failure controls (ADR-1246).
