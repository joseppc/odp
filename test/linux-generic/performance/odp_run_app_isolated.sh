#!/bin/sh
#
# Copyright (c) 2017, Linaro Limited
# All rights reserved.
#
# SPDX-License-Identifier:        BSD-3-Clause
#


TEST_DIR="${TEST_DIR:-$(dirname $0)}"
PERFORMANCE="$TEST_DIR/../../common_plat/performance"
ISOL_DIR="${TEST_DIR}/../../../scripts/task-isolation"
APPLICATION="${PERFORMANCE}/odp_pktio_perf${EXEEXT}"
APPLICATION_ARGS=""
APPLICATION_BASE="$(basename ${APPLICATION})"

cleanup(){
    pids=$(pgrep stress 2>/dev/null)
    [ "$pids" != "" ] && kill -9 $pids
}

print_usage() {
    cat <<-EOF
$0 [-i] [-n] [-h] [<application> <application args>]

 Run an application with or without isolation and background noise
 Flags:
  -h       Print this message
  -i       Isolate CPU prior to running application.
  -n       Create background noise (stress)

  <application>   targeted application
  <args>   targeted application arguments
  *Note* Default application is ${APPLICATION_BASE}

 Example:
  Isolate CPU, create background noise and run ${APPLICATION_BASE}:
  $0 -i -n

  Run ${APPLICATION_BASE}, w/o isolation but with background noise:
  $0 -n

  Run Myapp, without isolation but with background noise:
  $0 -n Myapp -s ome args
EOF
}

run() {
    local isolate=$1
    local noise=$2
    if [ ${isolate} -eq 1 ]; then
        [ ${noise} -eq 1 ] && noise_par="-n"
        echo Running ${APPLICATION_BASE} with isolation and background noise
        echo =====================================================
        $ISOL_DIR/isolate-task.sh ${noise_par} ${APPLICATION} \
                                   ${APPLICATION_ARGS} || exit 1
        #reset isolation
        $ISOL_DIR/isolate-cpu.sh -r
    else
        echo Running ${APPLICATION_BASE} without isolation
        echo =====================================================
        if [ ${noise} -eq 1 ]; then
            local nr=$(grep processor /proc/cpuinfo | wc -l)
            echo " Creating background noise..."
            stress -c $nr  2>&1 >/dev/null &
        fi
        ${APPLICATION} ${APPLICATION_ARGS} || exit 2
    fi
}

trap cleanup INT EXIT
ISOLATE=0
NOISE=0
while getopts hni arguments
do
    case $arguments in
        h)
            print_usage
            exit 0
            ;;
        n)
            NOISE=1
            if [ $(which stress > /dev/null 2>&1) ]; then
                echo "'stress' not found, bailing" >&2
                exit 3
            fi
            ;;
        i)
            ISOLATE=1
            ;;
        *)
            print_usage
            exit 1
            ;;
    esac
done
#Remove flags
shift $((OPTIND-1))

if [ $# -gt 0 ]; then
    APPLICATION="$1"
    shift
fi
[ $# -gt 0 ] && APPLICATION_ARGS=$*

run ${ISOLATE} ${NOISE}
