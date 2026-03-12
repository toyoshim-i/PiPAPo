# Commit Description Best Practices

This project uses short, scoped subjects plus clear bodies that explain
behavior changes and how they were validated.

## 1) Commit message structure

Use this shape:

```text
<scope>: <one-line summary in imperative mood>

<why this change is needed>
<what changed, focusing on behavior and risks>
<how you verified the change>
<extra context only if needed>

Co-Authored-By: <Agent name> (<model name>) [<optional valid email>]
```

### Rules

- Keep the first line short and specific.
- Leave one blank line after the subject.
- Wrap body lines to about 72 columns.
- Prefer "why + behavior impact" over implementation trivia.
- Keep one logical change per commit when possible.

## 2) Subject line convention in PPAP

Recent history often uses optional scope prefixes. Examples:

- `build: ...`
- `docs: ...`
- `exec: ...`
- `test: ...`
- `trace: ...`
- `signal: ...`
- `subsys: ...`
- `target: ...`
- `debug: ...`

If multiple areas are touched equally, choose the dominant behavior change
or use an unscoped subject.

## 3) What to include in the body

Include the details reviewers and future maintainers need:

- Previous behavior (or bug).
- New behavior after this commit.
- Any compatibility or regression risk.
- Follow-up work if this is part of a larger series.

Avoid:

- Repeating obvious diffs ("renamed X to Y") without why.
- Large narrative text not tied to behavior changes.

## 4) Verification in body

Include verification details in the commit body instead of a required trailer.
Keep it concise and concrete.

Examples:

```text
Verified with ./scripts/test.sh --all
Verified with ./scripts/run.sh --test qemu_m68k
Verified by building qemu_m68k target and checking boot output
```

If tests were not run, be explicit:

```text
Not verified by running tests (docs-only change)
```

## 5) Co-Authored-By protocol

Add `Co-Authored-By:` trailers when another contributor materially helped
create the commit (code, design, debugging, or substantial text).

### Placement

- Put trailers at the very end of the commit message.
- Keep a blank line between the body and trailers.
- One trailer line per contributor.

### Formats

For human contributors (standard git trailer format):

```text
Co-Authored-By: Jane Doe <jane@example.com>
```

For AI agent-assisted commits:

```text
Co-Authored-By: <Agent name> (<model name>) [<optional valid email>]
```

Current local example:

```text
Co-Authored-By: Codex (GPT-5.3-Codex)
```

If multiple co-authors exist, include all of them, one per line.

## 6) Quick pre-commit checklist

1. Does the subject describe the behavioral change clearly?
2. Does the body explain why this change exists?
3. Does the body include how the change was verified (or why not)?
4. Did affected tests pass (or is there a clear reason they were not run)?
5. Are affected documents updated?
6. Are `Co-Authored-By:` trailers present when applicable?
7. Is this commit scoped tightly enough to review easily?
