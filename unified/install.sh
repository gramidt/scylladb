#!/bin/bash
#
# Copyright (C) 2020-present ScyllaDB
#

#
# SPDX-License-Identifier: AGPL-3.0-or-later
#

set -e

if [ -z "$BASH_VERSION" ]; then
    echo "Unsupported shell, please run this script on bash."
    exit 1
fi

print_usage() {
    cat <<EOF
Usage: install.sh [options]

Options:
  --root /path/to/root     alternative install root (default /)
  --prefix /prefix         directory prefix (default /usr)
  --python3 /opt/python3   path of the python3 interpreter relative to install root (default /opt/scylladb/python3/bin/python3)
  --housekeeping           enable housekeeping service
  --nonroot                install Scylla without required root priviledge
  --sysconfdir /etc/sysconfig   specify sysconfig directory name
  --supervisor             enable supervisor to manage scylla processes
  --supervisor-log-to-stdout logging to stdout on supervisor
  --without-systemd         skip installing systemd units
  --help                   this helpful message
EOF
    exit 1
}

check_usermode_support() {
    user=$(systemctl --help|grep -e '--user')
    [ -n "$user" ]
}

root=/
housekeeping=false
nonroot=false
supervisor=false
supervisor_log_to_stdout=false
without_systemd=false

while [ $# -gt 0 ]; do
    case "$1" in
        "--root")
            root="$2"
            shift 2
            ;;
        "--prefix")
            prefix="$2"
            shift 2
            ;;
        "--housekeeping")
            housekeeping=true
            shift 1
            ;;
        "--python3")
            python3="$2"
            shift 2
            ;;
        "--nonroot")
            nonroot=true
            shift 1
            ;;
        "--sysconfdir")
            sysconfdir="$2"
            shift 2
            ;;
        "--supervisor")
            supervisor=true
            shift 1
            ;;
        "--supervisor-log-to-stdout")
            supervisor_log_to_stdout=true
            shift 1
            ;;
        "--without-systemd")
            without_systemd=true
            shift 1
            ;;
        "--help")
            shift 1
            print_usage
            ;;
        *)
            print_usage
            ;;
    esac
done

if [ ! -d /run/systemd/system/ ] && ! $supervisor; then
    echo "systemd is not detected, unsupported distribution."
    exit 1
fi

has_java=false
if [ -x /usr/bin/java ]; then
    javaver=$(/usr/bin/java -version 2>&1|head -n1|cut -f 3 -d " ")
    if [[ "$javaver" =~ ^\"1.8.0 || "$javaver" =~ ^\"11.0. ]]; then
        has_java=true
    fi
fi
if ! $has_java; then
    echo "Please install openjdk-8 or openjdk-11 before running install.sh."
    exit 1
fi

if [ -z "$prefix" ]; then
    if $nonroot; then
        prefix=~/scylladb
    else
        prefix=/opt/scylladb
    fi
fi
rprefix=$(realpath -m "$root/$prefix")

if [ -f "/etc/os-release" ]; then
    . /etc/os-release
fi

if [ -z "$sysconfdir" ]; then
    sysconfdir=/etc/sysconfig
    if ! $nonroot; then
        if [ "$ID" = "ubuntu" ] || [ "$ID" = "debian" ]; then
            sysconfdir=/etc/default
        fi
    fi
fi

if [ -z "$python3" ]; then
    python3=$prefix/python3/bin/python3
fi

scylla_args=()
jmx_args=()
args=()

if $housekeeping; then
    scylla_args+=(--housekeeping)
fi
if $nonroot; then
    scylla_args+=(--nonroot)
    jmx_args+=(--nonroot)
    args+=(--nonroot)
fi
if $supervisor; then
    scylla_args+=(--supervisor)
    jmx_args+=(--packaging)
fi
if $supervisor_log_to_stdout; then
    scylla_args+=(--supervisor-log-to-stdout)
fi
if $without_systemd; then
    scylla_args+=(--without-systemd)
    jmx_args+=(--without-systemd)
fi

(cd $(readlink -f scylla); ./install.sh --root "$root" --prefix "$prefix" --python3 "$python3" --sysconfdir "$sysconfdir" ${scylla_args[@]})

(cd $(readlink -f scylla-python3); ./install.sh --root "$root" --prefix "$prefix" ${args[@]})

(cd $(readlink -f scylla-jmx); ./install.sh --root "$root" --prefix "$prefix"  --sysconfdir "$sysconfdir" ${jmx_args[@]})

(cd $(readlink -f scylla-tools); ./install.sh --root "$root" --prefix "$prefix" ${args[@]})

install -m755 uninstall.sh -Dt "$rprefix"

if ! $supervisor && $nonroot && ! check_usermode_support; then
    echo "WARNING: This distribution does not support systemd user mode, please configure and launch Scylla manually."
fi
