#!/bin/bash
#
# This script generates checkpoints on the IBM burst buffer disk, and then
# starts transferring them to GPFS before exiting.  Then, the scr_poststage.sh
# script gets run after the transfers finish, and finalizes them.
#
# Example:
#
# # Allocate one node with 100GB burst buffer, run test_scr_poststage.sh to
# # create and begin transferring checkpoints, and after the transfers finish
# # (after the job ends), call scr_poststage.sh to finalize them.
# bsub -nnodes 1 -stage storage=100:out=,scr_poststage.sh lexec jsrun -r 1 test_scr_poststage.sh
#

# Where do you want to write your checkpoints to?  This should be somewhere in
# on the burst buffer filesystem.

BB_DIR="$BBPATH/my_checkpoints"

# Where do you want your checkpoints to transfer to in GPFS?
GPFS_DIR="/p/gpfs1/$(whoami)"
# GPFS_DIR=""

if [ -z "$BBPATH" ] ; then
    echo "BBPATH isn't set in the script, please set it"
    exit
fi

if [ -z "$GPFS_DIR" ] ; then
    echo "Please set \$GPFS_DIR in test_scr_poststage.sh"
    exit
fi

PATH=$PATH:~/post_stage5/build/examples/

if ! which test_api &> /dev/null ; then
    echo "Please add the path to SCR's 'test_api' binary into your \$PATH or into test_scr_poststage.sh itself"
    exit
fi

function make_scr_conf {
    echo "
SCR_COPY_TYPE=FILE

SCR_CLUSTER_NAME=`hostname`

SCR_FLUSH=1

STORE=$BB_DIR GROUP=NODE COUNT=1 FLUSH=BBAPI_POSTSTAGE

CKPT=0 INTERVAL=1 GROUP=NODE   STORE=$BB_DIR TYPE=PARTNER

SCR_CNTL_BASE=$BB_DIR
SCR_CACHE_BASE=$BB_DIR

CNTLDIR=$BB_DIR BYTES=10MB
CACHEDIR=$BB_DIR BYTES=10MB
SCR_DEBUG=10"
}

make_scr_conf > /tmp/test_scr_poststage.config

# Bypass mode is default - disable it to use AXL
export SCR_CACHE_BYPASS=0

mkdir -p $BB_DIR $GPFS_DIR

cd $GPFS_DIR

# Double check we're using GPFS
FS="$(stat --file-system --format=%T $GPFS_DIR)"
if [ "$FS" != "gpfs" ] ; then
    echo "GPFS_DIR must be on a GFPS filesystem (got $FS filesystem)"
    exit 1
fi

# Start the checkpoints
SCR_CONF_FILE=/tmp/test_scr_poststage.config test_api --times=1 --size=1G
