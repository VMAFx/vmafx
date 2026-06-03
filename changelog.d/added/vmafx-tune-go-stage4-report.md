`vmafx-tune-go report` — Stage 4: new subcommand renders Markdown or self-contained
HTML from one or more `compare` / `ladder` JSON output files.
`pkg/report/multi.go` adds `MultiReport`, `CompareWirePayload`, `LadderWirePayload`,
`RenderMarkdownMulti`, and `RenderHTMLMulti` (ADR-0770).
