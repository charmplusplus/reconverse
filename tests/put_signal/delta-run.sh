#!/bin/bash
# Put with signal on Slingshot (cxi): correctness and put-and-notify latency.
# Written for NCSA Delta CPU nodes; any Slurm machine with cxi and PMIx works.
#
# Build both trees on a login node first (standalone reconverse, LCI fetched):
#   module load cmake/3.31.8; export FI_PROVIDER=cxi
#   for t in Debug Release; do
#     cmake -S <reconverse> -B build-$t -DCMAKE_BUILD_TYPE=$t \
#       -DRECONVERSE_BUILD_TESTS=ON -DRECONVERSE_AUTOFETCH_LCI2=ON \
#       -DLCI_NETWORK_BACKENDS=ofi "-DRECONVERSE_TEST_LAUNCHER=srun;--mpi=pmix"
#     cmake --build build-$t -j8 --target put_signal put_signal_pingpong
#   done
# Then submit (two nodes; the one-node runs use the first):
#   sbatch -A <account>-delta-cpu -p cpu-interactive -N 2 --ntasks-per-node=2 \
#     -c 16 --mem=32g -t 00:30:00 \
#     --export=ALL,DBG=<abs path>/build-Debug,REL=<abs path>/build-Release \
#     delta-run.sh
#
# What it does:
# 1. prints the libfabric version and the cxi provider's ordering attributes;
# 2. runs tests/put_signal (every payload checked when its signal arrives) on
#    one node (2 processes) and on two nodes, each with one LCI device and with
#    +lci_ndevices 2; on LCI's own emulation of the signal (the default on cxi
#    unless libfabric >= 2.5 and FI_CXI_ENABLE_WRITEDATA=1), on reconverse's
#    (+lci_put_signal_emulate), on Converse's one-message path
#    (+lci_no_put_signal) and, where libfabric allows, on cxi's native
#    fi_writedata;
# 3. measures put-and-notify latency between two nodes with the Release build,
#    5 runs per configuration, and prints the median one-way time per size.
set -u
ulimit -c 0
export FI_PROVIDER=cxi
: "${DBG:?set DBG to the Debug build directory}"
: "${REL:?set REL to the Release build directory}"
RUNS=${RUNS:-5}
PS=tests/put_signal/reconverse_put_signal
PP=tests/put_signal/reconverse_put_signal_pingpong
ONE="srun --mpi=pmix -N 1 -n 2 -c 16 --cpu-bind=none"
TWO="srun --mpi=pmix -N 2 --ntasks-per-node=1 -c 16 --cpu-bind=none"
FAILED=0

echo "== $(date) job ${SLURM_JOB_ID:-none} nodes ${SLURM_JOB_NODELIST:-?}"
fi_info --version
LFV=$(fi_info --version | awk '/^libfabric:/ {print $2}')
LFMAJ=${LFV%%.*}
LFMIN=$(echo "$LFV" | cut -d. -f2)
if [ "$LFMAJ" -gt 2 ] || { [ "$LFMAJ" -eq 2 ] && [ "$LFMIN" -ge 5 ]; }; then
  WRITEDATA=1
else
  WRITEDATA=0
  echo "libfabric $LFV is older than 2.5: LCI does not use cxi's" \
       "fi_writedata, so the native path is not run"
fi
fi_info -p cxi -v | grep -E "msg_order|inject_size" | sort | uniq -c

# run <label> <env assignments or -> <launcher...> <binary and args...>
run() {
  local label=$1 envs=$2
  shift 2
  echo "== $label"
  local out
  if [ "$envs" = - ]; then
    out=$(timeout 300 "$@" < /dev/null 2>&1)
  else
    out=$(env $envs timeout 300 "$@" < /dev/null 2>&1)
  fi
  local rc=$?
  echo "$out" | grep -E "Starting Reconverse|put with signal:|put_signal|Reason|CmiAlloc" | head -12
  if ! echo "$out" | grep -q "every signal PASS\|65536 bytes: put with signal"; then
    echo "FAILED (rc=$rc): $label"
    echo "$out" | tail -20
    FAILED=$((FAILED + 1))
  fi
}

V=+lci_put_signal_verbose
WD="FI_CXI_ENABLE_WRITEDATA=1 LCI_ATTR_OFI_PROVIDER_NAME=cxi"
for where in ONE TWO; do
  L=${!where}
  run "$where 2x2 default" - $L $DBG/$PS +pe 4 $V
  run "$where 2x4 +lci_ndevices 2" - $L $DBG/$PS +pe 8 +lci_ndevices 2 $V
  run "$where 2x8 window 8, 20000 puts" - $L $DBG/$PS +pe 16 20000 8 $V
  run "$where 2x4 +lci_ndevices 2 +lci_put_signal_emulate" - \
      $L $DBG/$PS +pe 8 +lci_ndevices 2 +lci_put_signal_emulate $V
  run "$where 2x2 +lci_no_put_signal" - $L $DBG/$PS +pe 4 +lci_no_put_signal $V
  if [ $WRITEDATA = 1 ]; then
    run "$where 2x4 +lci_ndevices 2, native fi_writedata" "$WD" \
        $L $DBG/$PS +pe 8 +lci_ndevices 2 $V
  fi
done

# Latency: two nodes, one PE each, Release build.
lat() {
  local label=$1 envs=$2
  shift 2
  echo "== latency: $label ($RUNS runs, median one-way us)"
  local all=""
  for i in $(seq "$RUNS"); do
    if [ "$envs" = - ]; then
      all+=$(timeout 300 $TWO $REL/$PP +pe 2 2000 200 "$@" < /dev/null 2>&1)
    else
      all+=$(env $envs timeout 300 $TWO $REL/$PP +pe 2 2000 200 "$@" < /dev/null 2>&1)
    fi
    all+=$'\n'
  done
  echo "$all" | grep -m1 "put with signal:"
  echo "$all" | awk '/bytes: put with signal/ {
      gsub(",", ""); print $2, $7, $10 }' |
    sort -k1,1n -k2,2n |
    awk -v runs="$RUNS" '{ s[$1] = s[$1] " " $2; m[$1] = m[$1] " " $3 }
      END { for (k in s) {
              n = split(s[k], a, " "); split(m[k], b, " ");
              asort(a); asort(b);
              printf "%7s B: put with signal %8.2f  message %8.2f  (%d runs)\n",
                     k, a[int((n + 1) / 2)], b[int((n + 1) / 2)], n } }' |
    sort -n
}
lat "LCI's emulation (cxi default)" - +lci_put_signal_verbose
lat "reconverse's emulation (+lci_put_signal_emulate)" - \
    +lci_put_signal_emulate +lci_put_signal_verbose
if [ $WRITEDATA = 1 ]; then
  lat "native fi_writedata" "$WD" +lci_put_signal_verbose
fi

echo "== $(date) done, $FAILED failed"
