#!/bin/bash
# bench_configs.sh -- (re)create the raft benchmark configs.
#
# Written next to examples/raft/raft.json because DetSim resolves the tracee
# paths relative to the config file. The names are gitignored.
#
#   .bench-nohook.json : no UserCheck / UserState (state machine only)
#   .bench-hook.json   : UserCheck + UserState from plugins/check.so
#
# A second checker can be supplied to compare two plugin builds:
#   bash bench_configs.sh /path/to/other/check.so   -> .bench-checkb.json
#
# Logging is off (Loglevel 0): per-state log lines cost more than the hooks
# this benchmark measures, and would mask the difference.
set -u
D=${DETSIM_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
RAFT=$D/examples/raft
OTHER=${1:-}

python3 - "$RAFT/raft.json" "$RAFT" "$OTHER" <<'PY'
import json, os, sys
src, out, other = sys.argv[1], sys.argv[2], sys.argv[3]
base = json.load(open(src))
base["Loglevel"] = 0

def write(name, hooks):
    c = json.loads(json.dumps(base))
    if hooks:
        c["UserCheck"] = hooks
        c["UserState"] = hooks
    else:
        c.pop("UserCheck", None)
        c.pop("UserState", None)
    path = os.path.join(out, ".bench-%s.json" % name)
    json.dump(c, open(path, "w"), indent=1)
    print(path)

write("nohook", None)
write("hook", "./plugins/check.so")
if other:
    write("checkb", other)
PY
