#!/bin/zsh
set -u

usage() {
    cat <<EOF
Usage: $0 <eden-cli> <exefs main> <sdl config ini> [seconds] [VAR=value...]

Boots Mario Kart World in eden-cli, drives the menus with a key script
(L+R, then A: Grand Prix, Mario, Mushroom Cup, 50cc, OK), holds A in the
race and prints the race frame rate. The VAR=value arguments are passed
to eden-cli as environment variables.

The config needs the default keyboard bindings (A on the A key, L on F,
R on H). Output goes to ./mkw_race_bench/<timestamp>.
EOF
    exit 1
}

[[ $# -lt 3 ]] && usage
binary=$1
game=$2
config=$3
seconds=${4:-300}
shift $(( $# < 4 ? $# : 4 ))

out=./mkw_race_bench/$(date +%Y%m%d-%H%M%S)
mkdir -p $out/frames

keys="f780:9,f780:11,f790:-9,f790:-11"
for ((frame = 850; frame <= 1800; frame += 40)); do
    keys="$keys,f$frame:4,f$((frame + 4)):-4"
done
keys="$keys,f3700:4"
print -r -- $keys > $out/keys.txt

environment=(EDEN_NX2_FPS_LOG=1 EDEN_NX2_AUTO_STUB=1 EDEN_NX2_KEY_SCRIPT=@$out/keys.txt
             EDEN_NX2_FRAME_CAPTURE=$out/frames EDEN_NX2_FRAME_CAPTURE_EVERY=600)
if [[ -z ${LIBVULKAN_PATH:-} ]]; then
    moltenvk=$(dirname $binary)/eglantine.app/Contents/Frameworks/libMoltenVK.dylib
    [[ -f $moltenvk ]] && environment+=(LIBVULKAN_PATH=$moltenvk)
fi

env $environment "$@" $binary -c $config -g $game > $out/log.txt 2>&1 &
pid=$!
for ((elapsed = 0; elapsed < seconds; ++elapsed)); do
    kill -0 $pid 2>/dev/null || break
    sleep 1
done
kill $pid 2>/dev/null
sleep 2
kill -9 $pid 2>/dev/null

LC_ALL=C sed 's/\x1b\[[0-9;]*m//g' $out/log.txt | tr -d '\000' > $out/log_clean.txt
grep -a "FPS:" $out/log_clean.txt |
    sed -E 's/.*\[ *([0-9]+)\.[0-9]+\].*FPS: ([0-9.]+).*/\1 \2/' > $out/fps.txt
movie_end=$(grep -a "2560x1440.*end of stream" $out/log_clean.txt | head -1 |
    sed -E 's/^ *\[ *([0-9]+)\..*/\1/')
if grep -aq "PANIC" $out/log_clean.txt; then
    print "The game panicked, see $out/log_clean.txt"
fi
if [[ -z $movie_end ]]; then
    print "Did not reach the race in ${seconds} s, see $out/frames"
    exit 1
fi
awk -v start=$((movie_end + 15)) -v end=$((movie_end + 85)) '
    $1 >= start && $1 <= end && $2 > 0 { sum += $2; count++; if (min == "" || $2 < min) min = $2 }
    END { if (count) printf "Race FPS: %.1f average, %.1f minimum (%d samples)\n", sum / count, min, count }
' $out/fps.txt
print "Logs and frames: $out"
