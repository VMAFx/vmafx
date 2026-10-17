- CI: CodeQL C/C++ and Python now analyse on every pull request and push, as
  CodeQL Actions already did. A pull request that skipped one of them got a
  neutral `CodeQL` check ("configuration not found"), which the required-checks
  aggregator blocks since ADR-3293
  ([ADR-3294](docs/adr/3294-codeql-every-language-every-pr.md)).
