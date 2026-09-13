#!/usr/bin/env bash
# Captures the machine state that every published benchmark number must be
# read alongside. Writes a text report to stdout; bench_all.sh commits it
# next to the results.
#
# The point is not completeness, it is falsifiability: a reader who gets a
# different number should be able to see, from this file, which of the
# thirty things that affect latency was different on their machine.
set -uo pipefail

section() { printf '\n== %s ==\n' "$1"; }

printf '# miniex machine report\n'
printf 'generated: %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
printf 'host:      %s\n' "$(hostname)"
printf 'uname:     %s\n' "$(uname -a)"

section "git"
git rev-parse HEAD 2>/dev/null || echo "not a git repository"
if ! git diff --quiet 2>/dev/null; then
  echo "WORKING TREE DIRTY -- this result is not reproducible from a commit"
fi

section "compiler"
for cxx in "${CXX:-c++}" clang++ g++; do
  command -v "$cxx" >/dev/null 2>&1 && { printf '%s: ' "$cxx"; "$cxx" --version | head -1; }
done
command -v cmake >/dev/null && cmake --version | head -1

case "$(uname -s)" in
Linux)
  section "cpu"
  lscpu 2>/dev/null || cat /proc/cpuinfo | head -30

  section "frequency governor and turbo"
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -r "$f" ] && printf '%s: %s\n' "$f" "$(cat "$f")"
  done | head -8
  for f in /sys/devices/system/cpu/intel_pstate/no_turbo \
           /sys/devices/system/cpu/cpufreq/boost; do
    [ -r "$f" ] && printf '%s: %s\n' "$f" "$(cat "$f")"
  done
  echo "(governor should be 'performance'; turbo should be OFF for repeatability)"

  section "c-states"
  [ -r /sys/module/intel_idle/parameters/max_cstate ] && \
    printf 'intel_idle.max_cstate: %s\n' "$(cat /sys/module/intel_idle/parameters/max_cstate)"
  ls /sys/devices/system/cpu/cpu0/cpuidle/ 2>/dev/null | head

  section "smt"
  [ -r /sys/devices/system/cpu/smt/control ] && \
    printf 'smt: %s\n' "$(cat /sys/devices/system/cpu/smt/control)"

  section "tsc"
  grep -o -m1 'constant_tsc\|nonstop_tsc\|tsc_reliable' /proc/cpuinfo | sort -u
  echo "(constant_tsc AND nonstop_tsc are required before publishing a number)"

  section "hugepages"
  grep -i huge /proc/meminfo
  [ -r /sys/kernel/mm/transparent_hugepage/enabled ] && \
    printf 'THP: %s\n' "$(cat /sys/kernel/mm/transparent_hugepage/enabled)"

  section "isolation"
  printf 'cmdline: %s\n' "$(cat /proc/cmdline)"
  echo "(look for isolcpus, nohz_full, rcu_nocbs on the benchmark cores)"

  section "numa"
  command -v numactl >/dev/null && numactl --hardware || echo "numactl not installed"

  section "kernel"
  uname -r
  ;;
Darwin)
  section "cpu"
  sysctl -n machdep.cpu.brand_string 2>/dev/null
  sysctl -n hw.physicalcpu hw.logicalcpu hw.ncpu 2>/dev/null | \
    paste -d' ' - - - | sed 's/^/physical logical ncpu: /'
  sysctl -n hw.perflevel0.physicalcpu hw.perflevel1.physicalcpu 2>/dev/null | \
    paste -d' ' - - | sed 's/^/performance-cores efficiency-cores: /'
  sysctl -n hw.memsize 2>/dev/null | awk '{printf "memory: %.1f GiB\n", $1/1073741824}'

  section "timing"
  cat <<'NOTE'
This is an Apple Silicon machine. The architectural counter (cntvct_el0)
ticks at approximately 24 MHz, so its resolution is ~41 ns -- coarser than
the operations being measured. Numbers from this machine are development
numbers: useful for A/B comparison, useless as absolute sub-microsecond
percentiles.

There is also no working equivalent of taskset/isolcpus, and the scheduler
may move a thread between performance and efficiency cores mid-run.

Publishable numbers come from x86-64 Linux. See docs/benchmarking.md.
NOTE
  ;;
*)
  section "cpu"
  echo "unknown platform: $(uname -s)"
  ;;
esac

section "engine build constants"
echo "(these are CMake cache variables; see cmake/MiniExchangeConfig.cmake)"
for var in MINIEX_MAX_ORDERS MINIEX_LADDER_TICKS MINIEX_RING_CAPACITY \
           MINIEX_TAP_CAPACITY MINIEX_COMPLETED_RING; do
  for cache in build/*/CMakeCache.txt; do
    [ -r "$cache" ] && grep -h "^${var}:" "$cache" 2>/dev/null | head -1
  done
done | sort -u
