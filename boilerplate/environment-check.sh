#!/usr/bin/env bash
# environment-check.sh - Preflight check for the container runtime project.
# Run with: sudo ./environment-check.sh

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}[PASS]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; FAILURES=$((FAILURES + 1)); }

FAILURES=0

echo "========================="
echo " Environment Check"
echo "========================="
echo ""

# 1. Check OS
if grep -qi 'ubuntu' /etc/os-release 2>/dev/null; then
    VERSION=$(grep VERSION_ID /etc/os-release | tr -d '"' | cut -d= -f2)
    pass "Ubuntu detected (version $VERSION)"
else
    fail "Not running Ubuntu. This project requires Ubuntu 22.04 or 24.04."
fi

# 2. Check kernel headers
if [ -d "/lib/modules/$(uname -r)/build" ]; then
    pass "Kernel headers found for $(uname -r)"
else
    fail "Kernel headers not found. Run: sudo apt install linux-headers-$(uname -r)"
fi

# 3. Check build-essential
if dpkg -s build-essential &>/dev/null; then
    pass "build-essential installed"
else
    fail "build-essential not installed. Run: sudo apt install build-essential"
fi

# 4. Check gcc
if command -v gcc &>/dev/null; then
    pass "gcc found: $(gcc --version | head -1)"
else
    fail "gcc not found"
fi

# 5. Check make
if command -v make &>/dev/null; then
    pass "make found: $(make --version | head -1)"
else
    fail "make not found"
fi

# 6. Check if running as root (needed for container operations)
if [ "$(id -u)" -eq 0 ]; then
    pass "Running as root"
else
    warn "Not running as root. The runtime requires root privileges."
fi

# 7. Check Secure Boot
if command -v mokutil &>/dev/null; then
    SB_STATE=$(mokutil --sb-state 2>/dev/null || echo "unknown")
    if echo "$SB_STATE" | grep -qi "disabled"; then
        pass "Secure Boot is disabled"
    elif echo "$SB_STATE" | grep -qi "enabled"; then
        fail "Secure Boot is enabled. Disable it in BIOS/UEFI to load kernel modules."
    else
        warn "Could not determine Secure Boot state"
    fi
else
    warn "mokutil not available — cannot check Secure Boot status"
fi

# 8. Check if we're in WSL
if grep -qi microsoft /proc/version 2>/dev/null; then
    fail "WSL detected. This project requires a native VM, not WSL."
else
    pass "Not running in WSL"
fi

# 9. Check clone/unshare capability
if unshare --pid --fork -- /bin/true 2>/dev/null; then
    pass "PID namespace creation works"
else
    warn "PID namespace creation failed (may need root)"
fi

echo ""
echo "========================="
if [ "$FAILURES" -gt 0 ]; then
    echo -e "${RED}$FAILURES issue(s) found. Fix them before proceeding.${NC}"
    exit 1
else
    echo -e "${GREEN}All checks passed!${NC}"
    exit 0
fi
