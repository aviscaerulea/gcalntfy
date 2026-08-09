## 開発環境

- Visual Studio 2022 または Build Tools（C++20、MSVC）
- vcpkg（`VCPKG_INSTALLATION_ROOT` 環境変数、または PATH 上で解決する。libebur128 はビルド時に自動導入）
- Task、PowerShell 7（`pwsh`）
- `rm`、`cp`、`mkdir` を含む coreutils（Git Bash 等。Taskfile が使用する）

## ビルド方法

```shell
task build    # ビルド（out/gcalntfy.exe）
task release  # リリースビルド + zip 作成
```

ビルド前に `.env` を作成する。次に GCP の OAuth クライアント情報を設定する。

```
GOOGLE_CLIENT_ID=xxxxxxxxxxxx.apps.googleusercontent.com
GOOGLE_CLIENT_SECRET=GOCSPX-xxxxxxxx
```

## MCP Tools: code-review-graph

This project has a knowledge graph over its C/C++ and PowerShell sources
(`src/`, `build.ps1`). When exploring those sources, use the code-review-graph
MCP tools before Grep/Glob/Read: the graph is cheaper in tokens and gives
structural context (callers, dependents) that file scanning cannot.

The graph does not cover Markdown, TOML, or build and CI definitions
(`Taskfile.yml`, GitHub Actions). Use Grep/Glob/Read for those, and for anything
the graph turns out to miss.

### Key tools

| Tool | Use when |
|------|----------|
| `mcp__code-review-graph__detect_changes_tool` | Reviewing code changes — gives risk-scored analysis |
| `mcp__code-review-graph__get_review_context_tool` | Need source snippets for review — token-efficient |
| `mcp__code-review-graph__get_impact_radius_tool` | Understanding blast radius of a change |
| `mcp__code-review-graph__get_affected_flows_tool` | Finding which execution paths are impacted |
| `mcp__code-review-graph__query_graph_tool` | Tracing callers, callees, imports, dependencies |
| `mcp__code-review-graph__semantic_search_nodes_tool` | Finding functions by name or keyword |
| `mcp__code-review-graph__get_architecture_overview_tool` | Understanding high-level codebase structure |
| `mcp__code-review-graph__refactor_tool` | Planning renames, finding dead code |

Embeddings are not generated for this project, so `semantic_search_nodes_tool`
matches on names and keywords rather than meaning.

### Workflow

The PostToolUse hook runs `code-review-graph update --skip-flows` on file
changes. Nodes and edges stay current; flow and community data do not. Refresh
them with `code-review-graph update` (without the flag) before relying on
`get_affected_flows_tool` or `get_architecture_overview_tool`.

1. Review code changes with `detect_changes_tool`.
2. Refresh flows, then understand impact with `get_affected_flows_tool`.

## 参考

@README.md
