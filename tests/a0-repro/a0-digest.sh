#!/usr/bin/env bash
# Summarize an a0-gate work directory after a RED run.
set -euo pipefail

usage() {
	printf 'usage: %s /tmp/a0gate.XXXX\n' "$(basename "$0")" >&2
	exit 2
}

[ "$#" = 1 ] || usage
WORK="${1%/}"
[ -d "$WORK" ] || { printf 'not a directory: %s\n' "$WORK" >&2; exit 2; }

section() {
	printf '\n== %s ==\n' "$1"
}

section "workdir"
du -sh "$WORK" 2>/dev/null || true
if find "$WORK" -maxdepth 1 -name '*.auxlog.txt' -type f | grep -q .; then
	printf 'largest aux logs:\n'
	du -h "$WORK"/*.auxlog.txt 2>/dev/null | sort -hr | head -8
fi

section "legs"
for log in "$WORK"/*.log; do
	[ -f "$log" ] || continue
	base="$(basename "$log")"
	case "$base" in
		*.client-rpc.log|*.dserver.log|*.auxlog.txt|*.shellspawn-retry1) continue ;;
	esac
	if grep -qE 'RESULT=OK|BOOT_OK' "$log"; then
		printf 'OK   %s\n' "$base"
	elif grep -q 'RESULT=HANG' "$log"; then
		printf 'HANG %s\n' "$base"
	elif ! grep -qE 'RESULT=|RUN_DONE|BOOT_OK' "$log"; then
		printf 'INCOMPLETE %s\n' "$base"
	else
		printf 'RED  %s\n' "$base"
	fi
done | sort

section "client RPC failures"
if find "$WORK" -maxdepth 1 -name '*.client-rpc.log' -type f | grep -q .; then
	grep -hE 'BAD (SEND|RECEIVE) STATUS|BAD RECEIVE MESSAGE|failed with code' "$WORK"/*.client-rpc.log 2>/dev/null \
		| sed -E 's/^\*\*\* [0-9]+:[0-9]+: //' \
		| sort | uniq -c | sort -nr
else
	printf 'none\n'
fi

section "server signatures"
if find "$WORK" -maxdepth 1 -name '*.dserver.log' -type f | grep -q .; then
	{ grep -hE 'MSTATE VIOLATION|PUSHREPLY|STASH|FLUSH|SENT-FALLBACK|RECV call=|Uncaught|panic|terminate|Resource deadlock|ERROR|Error' "$WORK"/*.dserver.log 2>/dev/null \
		| sed -E 's/^\[[^]]+\]//' \
		| sort | uniq -c | sort -nr | head -80 || true; }
else
	printf 'none\n'
fi

section "aux trace"
if find "$WORK" -maxdepth 1 -name '*.auxlog.txt' -type f | grep -q .; then
	python3 - "$WORK" <<'PY'
import collections
import glob
import re
import sys

work = sys.argv[1]
names = {
    1: "checkin",
    2: "checkout",
    7: "uidgid",
    8: "set_thread_handles",
    11: "fork_wait_for_child",
    12: "sigprocess",
    14: "interrupt_enter",
    15: "interrupt_exit",
    30: "pthread_kill",
    31: "pthread_canceled",
    34: "host_self_trap",
    35: "thread_self_trap",
    36: "mach_reply_port",
    38: "mach_msg_overwrite",
    39: "mach_port_deallocate",
    62: "semaphore_timedwait",
    71: "psynch_cvwait",
    73: "psynch_mutexwait",
    81: "ring_attach",
}
interesting = {2, 11, 14, 15, 30, 31, 62, 71, 73}
counts = collections.Counter()
call_re = re.compile(r"call=(\d+)(?:\(([^)]+)\))?")

def base_call(raw):
    return raw & 0x8000ffff

def name_for(base, explicit):
    if explicit:
        return explicit.replace("dserver_callnum_", "")
    return names.get(base, "unknown")

for path in glob.glob(f"{work}/*.auxlog.txt"):
    with open(path, errors="replace") as f:
        for line in f:
            msg = line.split("] ", 1)[-1].strip()
            m = call_re.search(msg)
            base = None
            explicit = None
            if m:
                raw = int(m.group(1))
                base = base_call(raw)
                explicit = m.group(2)

            if "PUSHREPLY" in msg:
                key = re.sub(r" htid=\d+| nstid=-?\d+", "", msg)
            elif "STASH" in msg or "FLUSH" in msg:
                key = re.sub(r" htid=\d+| nstid=-?\d+", "", msg)
                if base is not None:
                    key = call_re.sub(f"call={base}({name_for(base, explicit)})", key, count=1)
            elif "SENT-FALLBACK" in msg and base is not None:
                code = re.search(r"code=(-?\d+)", msg)
                key = f"SENT-FALLBACK call={base}({name_for(base, explicit)}) code={code.group(1) if code else '?'}"
            elif ("RECV call=" in msg or "REPLY-DISP call=" in msg) and base in interesting:
                if "RECV call=" in msg:
                    key = f"RECV call={base}({name_for(base, explicit)})"
                else:
                    disp = re.search(r"disp=([A-Z0-9_\\[\\]-]+)", msg)
                    key = f"REPLY-DISP call={base}({name_for(base, explicit)}) disp={disp.group(1) if disp else '?'}"
            elif "TIMER-FIRED" in msg:
                key = "TIMER-FIRED"
            else:
                continue
            counts[key] += 1

for key, count in counts.most_common(120):
    print(f"{count:7d} {key}")
PY
else
	printf 'none\n'
fi

section "client failure thread context"
if find "$WORK" -maxdepth 1 -name '*.client-rpc.log' -type f | grep -q . \
	&& find "$WORK" -maxdepth 1 -name '*.auxlog.txt' -type f | grep -q .; then
	python3 - "$WORK" <<'PY'
import collections
import glob
import os
import re
import sys

work = sys.argv[1]
fail_re = re.compile(r"^\*\*\* ([0-9]+):([0-9]+): (.*BAD (?:SEND|RECEIVE) STATUS: -?[0-9]+.*)\*\*\*$")
field_re = re.compile(r"\b([a-z]+)=(-?[0-9]+)")
call_re = re.compile(r"call=(\d+)(?:\(([^)]+)\))?")
names = {
    1: "checkin",
    2: "checkout",
    7: "uidgid",
    8: "set_thread_handles",
    11: "fork_wait_for_child",
    12: "sigprocess",
    14: "interrupt_enter",
    15: "interrupt_exit",
    30: "pthread_kill",
    31: "pthread_canceled",
    34: "host_self_trap",
    35: "thread_self_trap",
    36: "mach_reply_port",
    38: "mach_msg_overwrite",
    39: "mach_port_deallocate",
    62: "semaphore_timedwait",
    71: "psynch_cvwait",
    73: "psynch_mutexwait",
    81: "ring_attach",
}

def base_call(raw):
    return raw & 0x8000ffff

def normalize_call(msg):
    m = call_re.search(msg)
    if not m:
        return msg
    raw = int(m.group(1))
    base = base_call(raw)
    explicit = m.group(2)
    name = explicit.replace("dserver_callnum_", "") if explicit else names.get(base, "unknown")
    return call_re.sub(f"call={base}({name})", msg, count=1)

def parse_fields(line):
    return {k: v for k, v in field_re.findall(line)}

def line_time(line):
    m = re.search(r"\[RPCTRACE ([0-9.]+)\]", line)
    return float(m.group(1)) if m else 0.0

failures = collections.defaultdict(list)
for client_path in sorted(glob.glob(f"{work}/*.client-rpc.log")):
    leg = os.path.basename(client_path).removesuffix(".client-rpc.log")
    with open(client_path, errors="replace") as f:
        for line in f:
            m = fail_re.match(line.strip())
            if not m:
                continue
            failures[leg].append((m.group(1), m.group(2), m.group(3).strip()))

if not failures:
    print("none")
    sys.exit(0)

for leg, items in failures.items():
    aux_path = os.path.join(work, f"{leg}.auxlog.txt")
    print(f"-- {leg} --")
    if not os.path.exists(aux_path):
        print("  no auxlog")
        continue

    wanted = {(pid, tid) for pid, tid, _ in items}
    matches = {key: [] for key in wanted}
    process_matches = collections.Counter()
    first_seen = {}
    last_seen = {}

    with open(aux_path, errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            fields = parse_fields(line)
            pairs = {
                (fields.get("nspid"), fields.get("nstid")),
                (fields.get("hpid"), fields.get("htid")),
            }
            for key in wanted:
                pid, tid = key
                if key in pairs:
                    msg = normalize_call(line.split("] ", 1)[-1].strip())
                    matches[key].append((line_time(line), lineno, msg))
                    first_seen.setdefault(key, lineno)
                    last_seen[key] = lineno
                elif fields.get("nspid") == pid or fields.get("hpid") == pid:
                    process_matches[key] += 1

    for pid, tid, detail in items:
        key = (pid, tid)
        print(f"  {pid}:{tid} {detail}")
        seen = matches[key]
        if not seen:
            same_proc = process_matches[key]
            if same_proc:
                print(f"    exact thread: no aux match; same process events: {same_proc}")
            else:
                print("    exact thread: no aux match; same process events: 0")
            continue

        calls = collections.Counter()
        for _, _, msg in seen:
            m = call_re.search(msg)
            if m:
                raw = int(m.group(1))
                base = base_call(raw)
                calls[f"{base}({names.get(base, 'unknown')})"] += 1
            elif "SENT-" in msg:
                calls[msg.split()[0]] += 1
            else:
                calls[msg.split()[0] if msg else "?"] += 1
        print(f"    exact thread events: {len(seen)} lines {first_seen[key]}..{last_seen[key]}")
        print("    top events: " + ", ".join(f"{name}={count}" for name, count in calls.most_common(8)))
        print("    last events:")
        for ts, lineno, msg in seen[-8:]:
            print(f"      L{lineno} t={ts:.6f} {msg}")
PY
else
	printf 'none\n'
fi

section "red tails"
for log in "$WORK"/*.log; do
	[ -f "$log" ] || continue
	base="$(basename "$log")"
	case "$base" in
		*.client-rpc.log|*.dserver.log|*.auxlog.txt|*.shellspawn-retry1) continue ;;
	esac
	grep -qE 'RESULT=OK|BOOT_OK' "$log" && continue
	printf '\n-- %s --\n' "$base"
	tail -n 20 "$log"
	for side in client-rpc dserver; do
		side_log="$WORK/${base%.log}.$side.log"
		[ -f "$side_log" ] || continue
		printf '\n-- %s --\n' "$(basename "$side_log")"
		tail -n 20 "$side_log"
	done
	aux_log="$WORK/${base%.log}.auxlog.txt"
	if [ -f "$aux_log" ]; then
		printf '\n-- %s --\n' "$(basename "$aux_log")"
		tail -n 40 "$aux_log"
	fi
done
