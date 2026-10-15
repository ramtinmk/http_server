# Pre-commit checks

The repository uses [`pre-commit`](https://pre-commit.com/) for fast checks that
should run before every commit. The hooks validate common text and data-file
mistakes and check C functions for excessive cyclomatic complexity with the
pinned Lizard dependency.

## Install and enable

Install the project-local environment and enable its hooks:

```bash
./scripts/setup_precommit.sh
```

This creates `.venv/` (ignored by Git), installs the pinned pre-commit and
Lizard versions through the Runflare PyPI mirror, and installs the Git hook.
Override the mirror with `PIP_INDEX_URL=...` when needed.

The hook runs automatically during `git commit`. It may update whitespace or
end-of-file formatting; stage those updates and retry the commit.

## Run manually

Run checks for staged files:

```bash
make pre-commit
```

Run the complete repository check when auditing the current tree:

```bash
make complexity
```

CI checks the files changed by each push or pull request. This avoids failing
unrelated changes on the repository's existing complexity and whitespace
baseline while still enforcing the hooks on every changed file.

The Lizard hook rejects C functions with cyclomatic complexity greater than 15.
A complex function should normally be split into smaller functions rather
than bypassing the hook. The existing full-tree findings and their recommended
refactoring order are documented in
[`complexity-baseline.md`](complexity-baseline.md).

The pre-commit hooks complement, rather than replace, the C build, `make lint`,
and the runtime test suites described in [`CONTRIBUTING.md`](../../CONTRIBUTING.md).
