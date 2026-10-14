#!/bin/sh
# run_ccs.sh SERVER CLIENT "LAUNCHER WORDS" [server args...]
# Starts the server on an OS-chosen port, reads the port from its output,
# runs the client against it, and reports the client's verdict. The server
# exits on the client's "quit" request; it is killed if it has not exited
# 30 s after the client finished.
server=$1; client=$2; launcher=$3; shift 3
out=$(mktemp -t ccs_server.XXXXXX)
$launcher "$server" "$@" ++server-port 0 > "$out" 2>&1 &
spid=$!
port=""
i=0
while [ $i -lt 300 ]; do
  port=$(sed -n 's/.*Server port = \([0-9][0-9]*\).*/\1/p' "$out" | head -1)
  [ -n "$port" ] && break
  if ! kill -0 $spid 2>/dev/null; then echo "server exited before listening:"; cat "$out"; exit 1; fi
  sleep 0.1; i=$((i+1))
done
if [ -z "$port" ]; then echo "server never printed its port:"; cat "$out"; kill -9 $spid 2>/dev/null; exit 1; fi
echo "server listening on port $port"
"$client" 127.0.0.1 "$port"
rc=$?
i=0
while kill -0 $spid 2>/dev/null && [ $i -lt 300 ]; do sleep 0.1; i=$((i+1)); done
if kill -0 $spid 2>/dev/null; then echo "server did not exit after quit; killing"; kill -9 $spid; rc=1; fi
echo "--- server output ---"; cat "$out"; rm -f "$out"
exit $rc
