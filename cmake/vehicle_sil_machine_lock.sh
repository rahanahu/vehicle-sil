#!/bin/sh
# SPDX-License-Identifier: MIT
#
#   vehicle_sil_machine_lock.sh <lock file> <name> <command> [<argument>...]
#
# Runs <command> holding a lock on <lock file> (flock), which the tests of every build on this
# computer share (vehicle_sil_add_scenario_tests(... MACHINE_LOCK <name>)). The command keeps the
# lock until it ends: it inherits the descriptor, as this script replaces itself with it.
#
# The file is opened for reading only, without creating it: Linux refuses to open another user's
# file in /tmp for creating (fs.protected_regular). A new file is made readable for every user.
# When the file cannot be opened (another user's file without read access, /tmp not writable),
# the command runs without the lock and this script says so; the same test of another build may
# then run at the same time.
lock=$1
name=$2
shift 2
if [ ! -e "$lock" ]; then
    (umask 0 && : >> "$lock") 2> /dev/null
fi
# "command": a redirection that fails does not end the shell.
if ! { [ -r "$lock" ] && command exec 9< "$lock"; }; then
    echo "[machine lock] cannot open $lock (another user's file, or no /tmp to write in): running without the lock" \
         "'$name', so the same test of another build on this computer may run at the same time"
    exec "$@"
fi
if ! flock -n 9; then
    echo "[machine lock] waiting for $lock: a test of another build on this computer holds '$name' (the wait counts" \
         "toward this test's CTest TIMEOUT)"
    flock 9
fi
exec "$@"
