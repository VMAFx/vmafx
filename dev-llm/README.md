# vmaf-dev-llm

Local-LLM helpers for developers working on the VMAFx fork — review,
commit-message drafting, and doc-block generation. Ollama-first (offline by
default); cloud providers (Anthropic, OpenAI-compatible) are opt-in via the
`cloud` extra.

**Not linked into libvmaf or shipped to end users.** This is dev tooling.

## Install

```bash
pip install -e dev-llm
# optional: pip install -e dev-llm[cloud]
```

Then make sure Ollama is running locally (`ollama serve`) and pull a
coder-capable model:

```bash
ollama pull qwen2.5-coder:7b
```

## Usage

```bash
# Code review of a single file (picks the right prompt by extension)
vmaf-dev-llm review --file core/src/feature/adm.c

# Draft a Conventional-Commits message from the staged diff
git add -p
vmaf-dev-llm commitmsg

# Draft Doxygen @brief/@param blocks for a function
vmaf-dev-llm docgen --file core/src/dnn/tensor_io.c --symbol vmaf_tensor_from_luma

# Draft a model card from an ONNX model and the repository safety allowlist
vmaf-dev-llm modelcard --onnx model/tiny/example.onnx --repo-root .
```

`commitmsg` resolves `git` to an absolute executable, invokes it without a
shell or external diff driver, closes the child process's standard input, and
enforces a 30-second wall-clock timeout. A missing executable, non-zero exit,
or timeout is reported with the captured Git diagnostic and exits with status
1; an empty staged diff exits with status 2.

`modelcard` reads the repository-root allowlist at
`core/src/dnn/op_allowlist.c` when `--repo-root` is supplied (the current
directory is the default). Its safety section reports any model operator not
present in that file.

Configuration lives at `~/.config/vmaf-dev-llm/config.yaml`:

```yaml
backend: ollama          # ollama | anthropic | openai
ollama:
  base_url: http://localhost:11434
  default_model: qwen2.5-coder:7b
  timeout_seconds: 120
prompts_dir: null        # null = use packaged prompts
```

## Layout

```text
dev-llm/
├── pyproject.toml
├── README.md
├── src/vmaf_dev_llm/
│   ├── cli.py            # typer entry point
│   ├── config.py         # config loader (yaml)
│   ├── ollama_client.py  # local Ollama HTTP client (stdlib urllib only)
│   ├── process.py        # shell-free subprocess boundary with hard timeout
│   └── prompts/          # versioned prompt templates
└── tests/
```
