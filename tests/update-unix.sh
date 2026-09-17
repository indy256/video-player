#!/bin/sh
set -eu
helper=$1
root=$(mktemp -d)
trap 'rm -rf -- "$root"' EXIT
stage="$root/.video-player-update-test"
mkdir "$stage"
target="$root/Video Player.AppImage"
printf 'old application' >"$target"
cat >"$stage/download" <<'APP'
#!/bin/sh
printf '%s' "$1" >"$1"
APP
sleep 30 &
old_pid=$!
video="$root/video ' & [1].restarted"
sh "$helper" "$target" "$stage/download" "$old_pid" "$video" &
helper_pid=$!
count=0
while [ ! -f "$stage/ready" ]; do
    count=$((count + 1))
    test "$count" -lt 10
    sleep 1
done
test "$(cat "$target")" = 'old application'
printf install >"$stage/commit"
sleep 1
test ! -f "$video"
kill "$old_pid"
wait "$old_pid" 2>/dev/null || true
wait "$helper_pid"
count=0
while [ ! -f "$video" ]; do
    count=$((count + 1))
    test "$count" -lt 10
    sleep 1
done
test "$(cat "$video")" = "$video"
test -x "$target"
echo 'Unix update replacement and restart passed.'
