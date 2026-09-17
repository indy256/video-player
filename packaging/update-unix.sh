#!/bin/sh
set -eu
target=$1
source=$2
player_pid=$3
video=$4
stage=$(dirname "$source")
exec >>"$stage/update.log" 2>&1
test "$(dirname "$stage")" = "$(dirname "$target")"
test -e "$target"
test -e "$source"
test ! -L "$target"
if [ -d "$target" ]; then
    case "$target" in /*.app) ;; *) exit 1 ;; esac
    test -x "$target/Contents/MacOS/VideoPlayer"
    test -x "$source/Contents/MacOS/VideoPlayer"
fi
printf ready >"$stage/ready"
count=0
while [ ! -f "$stage/commit" ]; do
    count=$((count + 1))
    if [ "$count" -gt 30 ]; then echo 'Installation was not committed.'; exit 1; fi
    sleep 1
done
count=0
while kill -0 "$player_pid" 2>/dev/null; do
    count=$((count + 1))
    if [ "$count" -gt 120 ]; then echo 'The player did not close in time.'; exit 1; fi
    sleep 1
done
if [ -d "$target" ]; then
    # Retain the old bundle until the prepared bundle is in place.
    mv "$target" "$stage/previous.app"
    if ! mv "$source" "$target"; then
        mv "$stage/previous.app" "$target"
        exit 1
    fi
    if [ -n "$video" ]; then /usr/bin/open -n "$target" --args "$video";
    else /usr/bin/open -n "$target"; fi
else
    chmod 755 "$source"
    mv -f "$source" "$target"
    if [ -n "$video" ]; then "$target" "$video" &
    else "$target" & fi
fi
echo 'Update installed.'
