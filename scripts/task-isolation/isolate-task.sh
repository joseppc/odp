#!/bin/bash
#
# Copyright (c) 2017, Linaro Limited
# All rights reserved.
#
# SPDX-License-Identifier:   BSD-3-Clause
#
# Script that passes command line arguments to odp_scheduling after,
# optionally, isolating CPU
#
# This script isolates a task on desired CPUs and
# optionally creates background noise.

print_usage() {
    cat <<-EOF
$0 [-c <cpu list>] [-d] [-h] [-n]  <application arg1, arg2, ...>

 Isolate CPU(s) from other tasks, kernel threads and IRQs
 and run an application on isolated CPUs
 Args:
  -c       List of CPUs to be isolated
  -d       Show debug printouts
  -h       Print this message
  -n       Create background noise (stress)

All CPU's, except CPU 0, are isolated unless '-c' specified
 Examples:
  Isolate CPU 1,2 and run application in the same.
  $0 -n -c 1,2 /some/path/application

  Isolate all possible CPUs and run applicatipon
  $0 /path/application
EOF
}

dlog() {
    [ $DEBUG ] && echo "$*"
}

die() {
    printf "Error: $*\n" >&2
    exit 1
}

trap cleanup INT EXIT

cleanup(){
    local pids=$(pgrep $MY_STRESS 2>/dev/null)
    local base=$(dirname $0)

    $base/isolate-cpu.sh -r
    [ "$pids" != "" ] && kill -9 $pids >/dev/null 2>&1
    kill -9 $CHILD >/dev/null 2>&1
    rm -f $MY_STRESS_PATH
}

wait_app_started () {
    local child=$1
    local ltasks=0

    while true; do
        sleep 0.01
        kill -0 $child 2>/dev/null || break
        tasks=$(ls /proc/$child/task | wc -l)
        [ $tasks -eq $ltasks ] && break
        ltasks=$tasks
    done
    dlog "app started, # threads:$ltasks"
}

create_noise() {
    local mpath=$1
    local nr=$(grep processor /proc/cpuinfo | wc -l)

    ln -sf $(which stress) $mpath || die "ln failed"
    $mpath -c $nr  2>&1 >/dev/null &
    disown $!
}

isolate_cpu(){
    local cpus=$1
    local base=$(dirname $0)
    $base/isolate-cpu.sh -c $cpus || die "$0 failed"
}

run_application() {
    local app="$1"

    dlog "Starting application: $app"
    $app&
    child=$!
    CHILD=$child

    echo $child >> /cpusets/user/tasks
    if [ $? -ne 0 ]; then
        kill -9 $child
        die "Failed to isolate task..."
    fi

    wait_app_started $child
    wait $child
}

check_prequesties() {
    local base=$(dirname $0)

    [ -e $base/isolate-cpu.sh ] || die "$base/isolate-cpu.sh not found!"
    [ $UID -eq 0 ] || die "You need to be root!"
    which stress > /dev/null 2>&1 || die "stress command not found, "\
                                         "please install stress"
}

##
# Script entry point
##

ISOL_CPUS="1-$(($(getconf _NPROCESSORS_ONLN) - 1))"

while getopts hdnc: arguments
do
    case $arguments in
        h)
            print_usage
            exit 0
            ;;
        d)
            DEBUG=1
            ;;
        n)
            NOISE=1
            ;;
        c)
            ISOL_CPUS=$OPTARG
            ;;
        *)
            print_usage
            exit 1
            ;;
    esac
done
# Remove all flags
shift $((OPTIND-1))

if ! [ "$1" ]; then
    print_usage
    exit 1
fi

#Isolate and optionally create noise
command="$*"
set -- $command

check_prequesties

MY_STRESS=stress-by-$$
MY_STRESS_PATH=/tmp/$MY_STRESS

isolate_cpu $ISOL_CPUS || die "isolate cpu failed!"
[ -z $NOISE ] || create_noise $MY_STRESS_PATH
run_application "$command"

exit $?
