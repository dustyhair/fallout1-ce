#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 FALLOUT_CE_BINARY FALLOUT_DATA_ROOT [OUTPUT_DIRECTORY]" >&2
    exit 2
fi

binary=$(realpath "$1")
data_root=$(realpath "$2")
core_test="$(dirname "$binary")/fallout-multiplayer-core-test"
if [[ $# -eq 3 ]]; then
    output_root=$(realpath -m "$3")
    if [[ -d "$output_root" && -n $(find "$output_root" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
        echo "campaign output directory must be empty: $output_root" >&2
        exit 2
    fi
    mkdir -p "$output_root"
else
    output_root=$(mktemp -d -t fallout-multiplayer-campaign.XXXXXX)
fi

for required in "$binary" "$data_root/master.dat" "$data_root/critter.dat" "$data_root/fallout.cfg"; do
    if [[ ! -e "$required" ]]; then
        echo "missing campaign input: $required" >&2
        exit 2
    fi
done
if [[ ! -x "$core_test" ]]; then
    echo "missing headless multiplayer gate: $core_test (configure with BUILD_TESTING=ON)" >&2
    exit 2
fi
if [[ ! -d "$data_root/data" ]]; then
    echo "missing campaign data directory: $data_root/data" >&2
    exit 2
fi

for role in host guest; do
    role_root="$output_root/$role"
    mkdir -p "$role_root"
    cp "$binary" "$role_root/fallout-ce"
    cp "$data_root/fallout.cfg" "$role_root/fallout.cfg"
    cp -a "$data_root/data" "$role_root/data"
    ln -s "$data_root/master.dat" "$role_root/master.dat"
    ln -s "$data_root/critter.dat" "$role_root/critter.dat"
done

base_port=${FALLOUT_CAMPAIGN_BASE_PORT:-46500}
scenario_index=0
summary="$output_root/summary.tsv"
printf 'scenario\thost_status\tguest_status\tresult\n' > "$summary"
if "$core_test" >"$output_root/multiplayer-core.log" 2>&1; then
    printf 'direct_trade_and_fault_core\t0\t0\tPASS\n' >> "$summary"
else
    printf 'direct_trade_and_fault_core\t1\t1\tFAIL\n' >> "$summary"
    echo "headless multiplayer campaign gate failed: $output_root/multiplayer-core.log" >&2
    exit 1
fi

run_scenario()
{
    label=$1
    shift
    port=$((base_port + scenario_index))
    scenario_index=$((scenario_index + 1))
    host_log="$output_root/host/$label.log"
    guest_log="$output_root/guest/$label.log"
    common=(--multiplayer-smoke-test "$@")

    echo "[$scenario_index] $label (port $port)"
    set +e
    (
        cd "$output_root/host"
        timeout 240s xvfb-run -a ./fallout-ce "--multiplayer-host=$port" "${common[@]}"
    ) >"$host_log" 2>&1 &
    host_pid=$!
    sleep 0.75
    (
        cd "$output_root/guest"
        timeout 240s xvfb-run -a ./fallout-ce "--multiplayer-join=127.0.0.1:$port" "${common[@]}"
    ) >"$guest_log" 2>&1
    guest_status=$?
    wait "$host_pid"
    host_status=$?
    set -e

    if [[ $host_status -ne 0 || $guest_status -ne 0 ]] \
        || ! grep -Eq 'MULTIPLAYER_(RECOVERY_)?SMOKE(_TEST)?_PASS role=host' "$host_log" \
        || ! grep -Eq 'MULTIPLAYER_(RECOVERY_)?SMOKE(_TEST)?_PASS role=guest' "$guest_log"; then
        printf '%s\t%s\t%s\tFAIL\n' "$label" "$host_status" "$guest_status" >> "$summary"
        echo "campaign scenario failed: $label" >&2
        echo "host log: $host_log" >&2
        echo "guest log: $guest_log" >&2
        exit 1
    fi
    printf '%s\t%s\t%s\tPASS\n' "$label" "$host_status" "$guest_status" >> "$summary"
}

# Together these fixtures cover script authority, companion/AI critter snapshot
# handling, inventory conservation, map loading, elevators, encounters, timed
# queues, dialogue, combat, and the durable Ending/save/load boundary.
run_scenario movement
run_scenario loot --multiplayer-smoke-scenario=loot
run_scenario transfer --multiplayer-smoke-scenario=transfer
run_scenario container --multiplayer-smoke-scenario=container
run_scenario quest --multiplayer-smoke-scenario=quest
run_scenario elevator --multiplayer-smoke-scenario=elevation
run_scenario map_transition --multiplayer-smoke-scenario=map-transition
run_scenario worldmap_encounter --multiplayer-smoke-scenario=worldmap-encounter
run_scenario timed_rest --multiplayer-smoke-scenario=rest \
    --multiplayer-smoke-rest-choice=until_morning --multiplayer-smoke-rest-interrupt
run_scenario scripted_combat --multiplayer-smoke-scenario=combat-script-status
run_scenario dialogue --multiplayer-smoke-scenario=dialogue-guest
run_scenario recovery --multiplayer-smoke-scenario=recovery

echo "MULTIPLAYER_COMPATIBILITY_CAMPAIGN_PASS scenarios=$scenario_index headless=1 output=$output_root"
