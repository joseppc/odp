#!/bin/bash
#
# Copyright (c) 2017, Linaro Limited
# All rights reserved.
#
# SPDX-License-Identifier:    BSD-3-Clause
#
# Script that passes command line arguments to odp_scheduling after,
# optionally, isolating CPU
#
# This script isolates desired CPUS, i.e.
# - Checks kernel cmdline and kernel config to determine
#   if the environment is optimised for isolated task execution;
# - Moves CPU interrupts, kernel threads, tasks etc. away from the
#   targeted CPU.
# *Note* CPU 0 cannot be isolated, i.e minimum 2 CPU's are required.


print_usage() {
cat <<-EOF
$0 [-h] [-a] [-c <cpu list>] [-l] [-r] [-d]

 Isolate CPU(s) from other tasks, kernel threads and IRQs
 Args:
  -h       Print this message
  -a       Isolate all CPUs (except CPU 0)
  -c       List of CPUs to be isolated.
  -l       Show isolation proporties
  -r       Reset isolation
  -d       Show debug printouts

 Examples:
  Isolate all CPU(s) (except 0)
  $0 -a

  Isolate CPUs 1-3
  $0 -c 1-3

  Isolate CPUs 1 and 4
  $0 -c 1,4
EOF
}

dlog() {
    [ $DEBUG ] && echo "$*"|| return 0
}

warn() {
    printf "Warning: $*\n" >&2
}

die() {
    printf "Error: $*\n" >&2
    exit 1
}

get_cpu_array() {
    [ $1 ] || die "$FUNCNAME internal error!"

    local cpus=""
    local oifs="$IFS"
    local a=$1;

    IFS=,
    a=$a;
    IFS="$oifs"

    for str in $a; do
        if [[ $str == *[\-]* ]]; then
            str=$(echo $str| sed 's/-/../g')
            str=$(eval echo {$str})
        fi

        if [ "$cpus" != "" ]; then
            cpus="$cpus $str"
        else
            cpus=$str
        fi
    done

    echo $cpus
}

##
# Check kernel config and kernel cmdline for rcu callbacs and no_hz
# *Note* isolcpu= kernel cmdline option isolates CPUs from SMP balancing
#        If needed, this can be done via
#        cpusets/user/cpuset.sched_load_balance
##
check_kernel_config() {

    eval $(grep -o 'nohz_full=[^ ]*' /proc/cmdline)

    local configs="/proc/config.gz /boot/config-$(uname -r) /boot/config "
    dlog "Looking for Kernel configs; $configs "
    for config in $configs; do
        if [ -e $config ]; then
            dlog "Kenel configuration found:$config"
            break
        fi
    done

    local all_except_0="1-$(($(getconf _NPROCESSORS_ONLN) - 1))"
    if [ -r $config ]; then
        nohz_full=$(zgrep "CONFIG_NO_HZ_FULL_ALL=y" $config  2>/dev/null) \
            && nohz_full=$all_except_0
    else
        warn "Kernel config not found, only checking /proc/cmdline for"\
             " isolation features."
    fi

    if ! [ "$nohz_full" ]; then
        eval $(grep -o 'nohz_full=[^ ]*' /proc/cmdline)
    fi

    eval $(grep -o 'isolcpus=[^ ]*' /proc/cmdline)
    if [ -z "$isolcpus" ]; then
        warn "No CPU is isolated from kernel/user threads, isolcpus= is "\
             "not set in kernel cmdline."
    else
        gbl_isolated_cpus=$isolcpus
        export gbl_isolated_cpus
    fi

    if [ -z "$nohz_full" ]; then
        warn "No CPU is isolated from kernel ticks, CONFIG_NO_HZ_FULL_ALL=y" \
             "  not set in kernel, nor nohz_full= set in kernel cmdline."
    fi

    for i in `pgrep rcu` ; do taskset -pc 0 $i >/dev/null; done

    dlog "isolcpus:$isolcpus"
    dlog "nohz_full:$nohz_full"
    #dlog "rcu_nocbs:$rcu_nocbs"

    return 0
}

cpus_valid() {
    local cpus="$1"
    local isolated=$2
    local iarray=$(get_cpu_array $isolated)
    local carray=$(get_cpu_array $cpus)

    for c in $carray; do
        for i in $iarray; do
            if [ $i = $c ]; then
                yah=$i
            fi
        done
        [ -z "$yah" ] && return 1
    done

    return 0
}

check_prequesties() {
    dlog "Checking prequesties; user is root, kernel has cpuset support,"\
         " and commads; set, zgrep, getconf are available"
    [ $UID -eq 0 ] || die "You need to be root!"
    grep -q -s cpuset /proc/filesystems || die "Kernel does not support cpuset!"
    which getconf > /dev/null 2>&1 || die "getconf command not found, please "\
                                          "install getconf"
    which cset > /dev/null 2>&1 || die "cset command not found, please "\
                                       "install cpuset"
    which zgrep > /dev/null 2>&1 || die "zgrep command not found, please "\
                                        "install gzip"
}

shield_reset() {
    cset shield -r >/dev/null 2>&1
    sleep 0.1
}

shield_list() {
    sets="/cpusets/*/"
    for i in $sets ; do
        if ! [ -e $i ]; then
            continue
        fi
        printf "Domain %s cpus %s, running %d tasks\n" \
               $(basename $i) $(cat $i/cpuset.cpus) $(cat $i/tasks | wc -l)
    done
}

shield_cpus() {
    local cpus="$1"

    dlog "shielding CPU:s $cpus"

    #Reset and create new shield
    shield_reset
    out=$(cset shield -c $cpus -k on 2>&1)  || die "cset failed; $out"
    # Delay the annoying vmstat timer far away
    sysctl vm.stat_interval=120 >/dev/null

    # Shutdown nmi watchdog as it uses perf events
    sysctl -w kernel.watchdog=0 >/dev/null

    # Pin the writeback workqueue to CPU0
    #Fixme, check that /sys/bus is mounted?
    echo 1 > /sys/bus/workqueue/devices/writeback/cpumask

    # Disable load balancer.
    echo 0 > /cpusets/user/cpuset.sched_load_balance

    #Fixme, for now just send all irqs to core 0
    for affinity in /proc/irq/*/smp_affinity; do
        dlog "redirecting $affinity"
        echo 1 > $affinity 2>/dev/null || dlog "$affinity redirection failed."
    done


    #Fixme, not implemented.
    if [ $false ];  then
        for affinity in /proc/irq/*/smp_affinity; do
            local old_mask=$(cat $affinity)
            local new_mask=$((oldmask ^ cpus ))
            echo $new_mask > $affinity
        done
    fi
}

isolate_cpus() {

    local cpus="$1"
    local carray=$(get_cpu_array $cpus)

    #cset allows CPU 0 to be isolated, we don't, since
    #IRQs are routed to core 0
    for c in $carray; do
        if [ $c = 0 ]; then
        die "Selected CPU 0 is not a valid CPU!"
        fi
    done
    check_kernel_config

    if [ "$gbl_isolated_cpus" ]; then
        cpus_valid $cpus $gbl_isolated_cpus ||
            warn "Selected CPU '$cpus' is not inside isolated cpus "\
                 "array:$gbl_isolated_cpus"
    fi

    dlog "Isolating CPUs $cpus"

    shield_cpus $cpus

    # Verify cores empty
    for c in $(get_cpu_array $cpus); do
        running=$(ps ax -o pid,psr,comm | \
                         awk -v cpu="$c" '{if($2==cpu){print $3}}')
        if [ "$running" != "" ]; then
            warn "Core $c not empty!"
            dlog "; running tasks:\n$running\n"
        fi
    done
}

##
# Script entry point
##
while getopts hdarlc: arguments
do
    case $arguments in
        h)
            print_usage
            exit 0
            ;;
        d)
            DEBUG=1
            ;;
        a)
            ISOL_CPUS="1-$(($(getconf _NPROCESSORS_ONLN) - 1))"
            ;;
        r)
            shield_reset
            exit 0
            ;;
        l)
            shield_list
            exit 0
            ;;
        c)
            [ "$ISOL_CPUS" ] || ISOL_CPUS=$OPTARG
            ;;
        *)
            print_usage
            exit 1
            ;;
    esac
done
#Remove all flags
shift $((OPTIND-1))

if ! [ $ISOL_CPUS ]; then
    print_usage
    exit 1
fi

check_prequesties
isolate_cpus $ISOL_CPUS || die "isolate_cpus failed."
