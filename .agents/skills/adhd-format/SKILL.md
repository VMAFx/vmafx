---
# SPDX-FileCopyrightText: 2026 lusoris <lusoris@pm.me>
#
# SPDX-License-Identifier: EUPL-1.2
name: adhd-format
description: Format complex technical reports, architectural reviews, and diagnostic outputs for high cognitive focus, executive clarity, and ADHD readability using visual hierarchy, bionic bolding, chunked lists, and alert callouts.
metadata:
  derived_from: "https://github.com/ayghri/i-have-adhd (MIT)"
---

# High-Focus Technical Formatting (`adhd-format`)

Adapted from [i-have-adhd](https://github.com/ayghri/i-have-adhd) by Ayoub
Ghriss and
[contributors](https://github.com/ayghri/i-have-adhd/graphs/contributors); MIT
licence.

Transform dense engineering outputs, audit findings, and architecture
documentation into high-bandwidth, ADHD-optimized visual deliverables.

## Core Formatting Principles

Forge-facing prose (issues, PR bodies, review comments, commit bodies) uses
social-text skill, which inherits this one.

1. **Lead with Bottom Line (BLUF)**:
   - State decision, status, or actionable conclusion in very first sentence.
   - Eliminate filler preambles (`Here is the report`, `Based on my analysis`).

2. **Visual Scannability & Anchor Bolding**:
   - Bold **first 2-4 words** or operative action of every bullet point.
   - Restrict paragraphs to **at most 2-3 sentences**.
   - Use high-contrast Markdown tables for multi-attribute comparisons.

3. **Strategic Alert Callouts**:
   - Use GitHub-style alerts sparingly for immediate cognitive anchoring:
     - `> [!IMPORTANT]` for blocking requirements or invariants.
     - `> [!WARNING]` for deprecations or breaking risks.
     - `> [!TIP]` for operational shortcuts and command patterns.

4. **Diagrammatic Synthesis**:
   - When explaining state transition, sequence, or lattice relationship,
     include compact Mermaid diagram ($\le 6$ nodes).

5. **Progressive Disclosure**:
   - Provide summary matrices first, actionable CLI commands second, and deep
     implementation details collapsed or linked below.

## Transformation Pattern

<!-- caveman:off -->
### ❌ Anti-Pattern (Cognitive Fatigue)

> "In evaluating the repository against HISS-04, we noticed that several
> functions in the compiler package have cyclomatic complexity values exceeding
> the threshold of 10. Specifically, `compileAstNode` has a complexity of 14,
> and its length is 92 lines which also violates the 60 line limit. We should
> refactor this into sub-functions."

### ✅ ADHD-Optimized Pattern

> ### 🚨 HISS-04 Complexity Infraction
>
> | Function | Cyclomatic | Limit | Func LOC | Limit | Status |
> | :--- | :--- | :--- | :--- | :--- | :--- |
> | `compileAstNode` | **14** | $\le 10$ | **92** | $\le 60$ | ❌ Blocked |
>
> **Immediate Action Required**:
>
> - **Extract node visit logic** into `visitExpression()` and
>   `visitStatement()`.
> - **Target reduction**: Reduce cyclomatic complexity from 14 to $\le 8$.
>
> ```mermaid
> flowchart LR
>     NODE["compileAstNode (LOC 92)"] --> EXPR["visitExpression (LOC 35)"]
>     NODE --> STMT["visitStatement (LOC 40)"]
> ```
<!-- caveman:on -->
