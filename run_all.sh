#!/usr/bin/env bash
# macOS/Linux용 바로가기입니다. 실제 내용은 run_all.py에 있습니다 (Windows: py run_all.py).
exec python3 "$(dirname "$0")/run_all.py" "$@"
