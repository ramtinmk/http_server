#!/bin/sh
# Install the project's local pre-commit and Lizard environment.
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VENV_DIR=${QUALITY_VENV_DIR:-"$ROOT_DIR/.venv"}
PIP_INDEX=${PIP_INDEX_URL:-https://mirror-pypi.runflare.com/simple}
export PIP_INDEX_URL="$PIP_INDEX"

if [ ! -x "$VENV_DIR/bin/python" ]; then
    python3 -m venv "$VENV_DIR"
fi

"$VENV_DIR/bin/python" -m pip install \
    --index-url "$PIP_INDEX_URL" \
    --disable-pip-version-check \
    --requirement "$ROOT_DIR/tools/quality/requirements.txt"

cd "$ROOT_DIR"
"$VENV_DIR/bin/pre-commit" install --install-hooks
printf 'Quality tools ready in %s\n' "$VENV_DIR"
