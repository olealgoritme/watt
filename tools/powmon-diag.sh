#!/bin/bash
# powmon-diag.sh — Dump hardware info for watt/powmon support
# Run as root: sudo ./tools/powmon-diag.sh

set -e

echo "=== powmon diagnostic report ==="
echo "Date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "Kernel: $(uname -r)"
echo "Arch: $(uname -m)"
echo ""

# CPU info
echo "=== CPU ==="
vendor=$(grep -m1 'vendor_id' /proc/cpuinfo | awk '{print $3}')
model=$(grep -m1 'model name' /proc/cpuinfo | sed 's/model name\s*: //')
family=$(grep -m1 'cpu family' /proc/cpuinfo | awk '{print $4}')
model_id=$(grep -m1 -w 'model' /proc/cpuinfo | awk '{print $3}')
stepping=$(grep -m1 'stepping' /proc/cpuinfo | awk '{print $3}')
cores=$(nproc)
sockets=$(lscpu 2>/dev/null | grep 'Socket(s)' | awk '{print $2}' || echo "?")
threads=$(lscpu 2>/dev/null | grep 'Thread(s) per core' | awk '{print $4}' || echo "?")

echo "Vendor:    $vendor"
echo "Model:     $model"
echo "Family:    $family"
echo "Model ID:  $model_id"
echo "Stepping:  $stepping"
echo "Cores:     $cores"
echo "Sockets:   $sockets"
echo "Threads/core: $threads"
echo ""

# RAPL powercap sysfs (for comparison)
echo "=== RAPL domains (powercap sysfs) ==="
if [ -d /sys/class/powercap ]; then
    for zone in /sys/class/powercap/intel-rapl:*; do
        [ -d "$zone" ] || continue
        name=$(cat "$zone/name" 2>/dev/null || echo "?")
        energy=$(cat "$zone/energy_uj" 2>/dev/null || echo "N/A")
        enabled=$(cat "$zone/enabled" 2>/dev/null || echo "?")
        echo "  $zone  name=$name  energy_uj=$energy  enabled=$enabled"

        # subzones
        for sub in "$zone"/intel-rapl:*; do
            [ -d "$sub" ] || continue
            sname=$(cat "$sub/name" 2>/dev/null || echo "?")
            senergy=$(cat "$sub/energy_uj" 2>/dev/null || echo "N/A")
            echo "    $sub  name=$sname  energy_uj=$senergy"
        done
    done
else
    echo "  (powercap sysfs not available)"
fi
echo ""

# Power limits from powercap
echo "=== Power limits (powercap) ==="
for zone in /sys/class/powercap/intel-rapl:*/constraint_*_power_limit_uw; do
    [ -f "$zone" ] || continue
    dir=$(dirname "$zone")
    name=$(cat "$dir/name" 2>/dev/null || echo "?")
    idx=$(basename "$zone" | sed 's/constraint_\([0-9]*\)_.*/\1/')
    limit=$(cat "$zone" 2>/dev/null || echo "N/A")
    window=$(cat "$dir/constraint_${idx}_time_window_us" 2>/dev/null || echo "N/A")
    echo "  $name constraint_$idx: ${limit}uW  window=${window}us"
done
echo ""

# MSR availability (requires msr module)
echo "=== MSR access ==="
if [ -c /dev/cpu/0/msr ] || modprobe msr 2>/dev/null; then
    check_msr() {
        local addr=$1 name=$2
        if rdmsr -p 0 "$addr" 2>/dev/null; then
            echo "  $name ($addr): $(rdmsr -p 0 "$addr")"
        else
            echo "  $name ($addr): NOT AVAILABLE"
        fi
    }
    if command -v rdmsr &>/dev/null; then
        check_msr 0x606 "RAPL_POWER_UNIT"
        check_msr 0x610 "PKG_POWER_LIMIT"
        check_msr 0x611 "PKG_ENERGY_STATUS"
        check_msr 0x619 "DRAM_ENERGY_STATUS"
        check_msr 0x639 "PP0_ENERGY_STATUS"
        check_msr 0x641 "PP1_ENERGY_STATUS"
        check_msr 0x64D "PLATFORM_ENERGY_STATUS"
        check_msr 0x64F "CORE_PERF_LIMIT_REASONS"
        # AMD
        check_msr 0xC0010299 "AMD_RAPL_POWER_UNIT"
        check_msr 0xC0010293 "AMD_PKG_ENERGY_STATUS"
        check_msr 0xC001029A "AMD_CORE_ENERGY_STATUS"
    else
        echo "  rdmsr not installed (apt install msr-tools)"
    fi
else
    echo "  /dev/cpu/0/msr not available (modprobe msr)"
fi
echo ""

# Thermal
echo "=== Thermal ==="
for tz in /sys/class/thermal/thermal_zone*; do
    [ -d "$tz" ] || continue
    type=$(cat "$tz/type" 2>/dev/null || echo "?")
    temp=$(cat "$tz/temp" 2>/dev/null || echo "?")
    echo "  $(basename "$tz"): type=$type  temp=${temp} (m°C)"
done
echo ""

# Battery (if laptop)
echo "=== Power supply ==="
for ps in /sys/class/power_supply/*; do
    [ -d "$ps" ] || continue
    type=$(cat "$ps/type" 2>/dev/null || echo "?")
    status=$(cat "$ps/status" 2>/dev/null || echo "?")
    power=$(cat "$ps/power_now" 2>/dev/null || echo "N/A")
    energy=$(cat "$ps/energy_now" 2>/dev/null || echo "N/A")
    echo "  $(basename "$ps"): type=$type  status=$status  power_now=${power}uW  energy_now=${energy}uWh"
done
echo ""

# GPU
echo "=== GPU ==="
if command -v nvidia-smi &>/dev/null; then
    echo "  NVIDIA:"
    nvidia-smi --query-gpu=name,power.draw,power.limit --format=csv,noheader 2>/dev/null | sed 's/^/    /'
fi
for card in /sys/class/drm/card*/device/hwmon/hwmon*/power1_average; do
    [ -f "$card" ] || continue
    val=$(cat "$card" 2>/dev/null || echo "N/A")
    echo "  amdgpu: $(dirname "$card" | sed 's|.*/drm/||;s|/.*||') power=${val}uW"
done
if [ ! -f /sys/class/drm/card*/device/hwmon/hwmon*/power1_average ] && ! command -v nvidia-smi &>/dev/null; then
    echo "  (no GPU power interface detected)"
fi
echo ""

# powmon module
echo "=== powmon module ==="
if lsmod | grep -q powmon; then
    echo "  Status: LOADED"
    if [ -c /dev/powmon ]; then
        echo "  Device: /dev/powmon present"
        if command -v ./tools/powmon-cli &>/dev/null || [ -x ./tools/powmon-cli ]; then
            echo ""
            echo "  --- powmon-cli info ---"
            ./tools/powmon-cli info 2>&1 | sed 's/^/  /'
        fi
    fi
else
    echo "  Status: NOT LOADED"
fi
echo ""
echo "=== end of report ==="
