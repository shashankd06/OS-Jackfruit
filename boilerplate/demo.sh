#!/usr/bin/env bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

if [ "$EUID" -ne 0 ]; then
  echo -e "${RED}Please run this script with sudo!${NC}"
  exit 1
fi

screenshot_pause() {
    echo ""
    echo -e "${YELLOW}╔════════════════════════════════════════════════════════╗${NC}"
    echo -e "${YELLOW}║  📸 SCREENSHOT $1: $2${NC}"
    echo -e "${YELLOW}║  Take your screenshot now, then press Enter.         ${NC}"
    echo -e "${YELLOW}╚════════════════════════════════════════════════════════╝${NC}"
    read -r
}

# ──────────────────────────────────────────
# CLEANUP — kill old state
# ──────────────────────────────────────────
echo -e "${CYAN}[CLEANUP] Killing old processes and resetting state...${NC}"
pkill -9 engine 2>/dev/null || true
sleep 1
rmmod monitor 2>/dev/null || true
rm -f /tmp/mini_runtime.sock
rm -rf logs
mkdir -p logs
dmesg -c > /dev/null 2>&1 || true
echo -e "${GREEN}[CLEANUP] Done.${NC}"
echo ""

# ──────────────────────────────────────────
# STEP 1: Load kernel module
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 1: Loading Kernel Module${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
insmod monitor.ko
ls -l /dev/container_monitor
echo -e "${GREEN}Kernel module loaded successfully.${NC}"
echo ""

# ──────────────────────────────────────────
# STEP 2: Start supervisor
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 2: Starting Supervisor${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
./engine supervisor ./rootfs-base > supervisor_output.log 2>&1 &
SUPERVISOR_PID=$!
sleep 1
echo -e "${GREEN}Supervisor started (PID: $SUPERVISOR_PID)${NC}"
echo ""

# ──────────────────────────────────────────
# SCREENSHOT 1: Multi-container supervision + CLI
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 3: Starting Containers${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}$ sudo ./engine start alpha ./rootfs-alpha \"/bin/sh -c 'echo Container alpha started; sleep 300'\" --soft-mib 48 --hard-mib 80${NC}"
./engine start alpha ./rootfs-alpha "/bin/sh -c 'echo Container alpha started; sleep 300'" --soft-mib 48 --hard-mib 80
echo ""
echo -e "${CYAN}$ sudo ./engine start beta ./rootfs-beta \"/bin/sh -c 'echo Container beta started; sleep 300'\" --soft-mib 64 --hard-mib 96${NC}"
./engine start beta ./rootfs-beta "/bin/sh -c 'echo Container beta started; sleep 300'" --soft-mib 64 --hard-mib 96
sleep 1
echo ""
echo -e "${CYAN}$ sudo ./engine ps${NC}"
./engine ps

screenshot_pause "1" "Multi-container supervision + Metadata + CLI"

# ──────────────────────────────────────────
# SCREENSHOT 2: Bounded-buffer logging
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 4: Bounded-Buffer Logging${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}$ sudo ./engine logs alpha${NC}"
./engine logs alpha
echo ""
echo -e "${CYAN}$ cat logs/alpha.log${NC}"
cat logs/alpha.log 2>/dev/null || echo "(log file contents shown above via engine logs)"

screenshot_pause "2" "Bounded-buffer logging pipeline"

# ──────────────────────────────────────────
# SCREENSHOT 3: Soft-limit + Hard-limit (memory kill)
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 5: Memory Limit Enforcement${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}$ sudo ./engine start memtest ./rootfs-alpha \"/memory_hog 8 500\" --soft-mib 24 --hard-mib 48${NC}"
./engine start memtest ./rootfs-alpha "/memory_hog 8 500" --soft-mib 24 --hard-mib 48
echo ""
echo "Waiting for memory_hog to breach limits..."
sleep 4
echo ""
echo -e "${CYAN}$ sudo ./engine ps${NC}"
./engine ps

screenshot_pause "3" "Memory kill — engine ps showing killed state"

# ──────────────────────────────────────────
# SCREENSHOT 4: Kernel dmesg logs
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 6: Kernel Monitor Logs (dmesg)${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}$ sudo dmesg | grep container_monitor${NC}"
dmesg | grep container_monitor

screenshot_pause "4" "Kernel dmesg — soft + hard limit events"

# ──────────────────────────────────────────
# SCREENSHOT 5: Scheduling Experiment 1 — CPU priorities
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 7: Scheduler Experiment 1${NC}"
echo -e "${BOLD}         CPU-bound: nice -10 vs nice 10${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}Running simultaneously:${NC}"
echo -e "${CYAN}$ sudo ./engine run cpu-hi ./rootfs-alpha \"/cpu_hog 20\" --nice -10 &${NC}"
echo -e "${CYAN}$ sudo ./engine run cpu-lo ./rootfs-beta \"/cpu_hog 20\" --nice 10 &${NC}"
echo ""
./engine run cpu-hi ./rootfs-alpha "/cpu_hog 20" --nice -10 &
PID_HI=$!
./engine run cpu-lo ./rootfs-beta "/cpu_hog 20" --nice 10 &
PID_LO=$!
wait $PID_HI $PID_LO 2>/dev/null || true
echo ""
echo -e "${CYAN}--- cpu-hi log (nice -10) ---${NC}"
./engine logs cpu-hi
echo ""
echo -e "${CYAN}--- cpu-lo log (nice  10) ---${NC}"
./engine logs cpu-lo

screenshot_pause "5" "Scheduling Experiment 1 — CPU priority comparison"

# ──────────────────────────────────────────
# SCREENSHOT 6: Scheduling Experiment 2 — CPU vs I/O
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 8: Scheduler Experiment 2${NC}"
echo -e "${BOLD}         CPU-bound vs I/O-bound${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}Running simultaneously:${NC}"
echo -e "${CYAN}$ sudo ./engine run cpu-test ./rootfs-alpha \"/cpu_hog 15\" --nice 0 &${NC}"
echo -e "${CYAN}$ sudo ./engine run io-test ./rootfs-beta \"/io_pulse 40 100\" --nice 0 &${NC}"
echo ""
./engine run cpu-test ./rootfs-alpha "/cpu_hog 15" --nice 0 &
PID_CPU=$!
./engine run io-test ./rootfs-beta "/io_pulse 40 100" --nice 0 &
PID_IO=$!
wait $PID_CPU $PID_IO 2>/dev/null || true
echo ""
echo -e "${CYAN}--- cpu-test log ---${NC}"
./engine logs cpu-test
echo ""
echo -e "${CYAN}--- io-test log ---${NC}"
./engine logs io-test

screenshot_pause "6" "Scheduling Experiment 2 — CPU vs I/O comparison"

# ──────────────────────────────────────────
# SCREENSHOT 7: Clean teardown
# ──────────────────────────────────────────
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${BOLD} STEP 9: Clean Shutdown${NC}"
echo -e "${BOLD}═══════════════════════════════════════════${NC}"
echo -e "${CYAN}$ sudo ./engine stop alpha${NC}"
./engine stop alpha 2>/dev/null || true
echo -e "${CYAN}$ sudo ./engine stop beta${NC}"
./engine stop beta 2>/dev/null || true
sleep 1
echo ""
echo -e "${CYAN}Terminating supervisor (PID $SUPERVISOR_PID)...${NC}"
kill -TERM $SUPERVISOR_PID 2>/dev/null || true
sleep 3
echo ""
echo -e "${CYAN}$ ps aux | grep -E 'engine|defunct'${NC}"
ps aux | grep -E 'engine|defunct' | grep -v grep || echo "No engine processes or zombies found — clean shutdown!"
echo ""
echo -e "${CYAN}$ sudo dmesg | tail -5${NC}"
dmesg | tail -5
echo ""
echo -e "${CYAN}$ sudo rmmod monitor${NC}"
rmmod monitor 2>/dev/null || true
echo -e "${GREEN}Kernel module unloaded.${NC}"

screenshot_pause "7" "Clean teardown — no zombies, module unloaded"

echo ""
echo -e "${GREEN}═══════════════════════════════════════════${NC}"
echo -e "${GREEN} ✅ DEMO COMPLETE — All 7 screenshots taken${NC}"
echo -e "${GREEN}═══════════════════════════════════════════${NC}"
