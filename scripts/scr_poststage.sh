#!/bin/bash
#
# SCR allows you to spawn off checkpoint transfers "in the background"
# that will finish some time after a job completes.  This saves you from
# using your compute time to transfer checkpoints.  You can do this by
# specifying XFER=BBAPI_POSTSTAGE for a storage descriptor in scr.conf.
# Currently, this is only supported on on IBM burst buffer nodes.
#
# This script is to be run as a 2nd-half post-stage script on an IBM
# system.  A 2nd-half post-stage script will run after all the job's burst
# buffers transfers have finished (which could be hours later after the job
# finishes).  This script will finalize any completed burst buffer checkpoint
# transfers so that they're visible to SCR.
#

# Please fill in BINDIR AND PREFIX below:

# The dir path containing the SCR binaries (scr_index and scr_flush_file)
BINDIR=""
# The dir path to your SCR PREFIX
PREFIX=""

# Path to where you want the scr_poststage.sh log
LOGFILE=/tmp/scr_poststage.log

if [ ! -d "$BINDIR" ] ; then
	echo "Please specify BINDIR in scr_poststage.sh" >> $LOGFILE
	exit
fi

if [ ! -d "$PREFIX" ] ; then
	echo "Please specify PREFIX in scr_poststage.sh" >> $LOGFILE
	exit
fi

export PATH="$PATH:$BINDIR"

echo "$(date '+%m/%d/%y %H:%m:%S') Begin post_stage" >> $LOGFILE
echo "Current index before finalizing:" >> $LOGFILE
scr_index -l -p $PREFIX >> $LOGFILE

# Finalize each dataset listed in `scr_index -l -p` that is NOT
# flushed.
#
# $ scr_index -l -p $PREFIX
# DSET VALID FLUSHED             CUR NAME
#   2 NO                          ckpt.2
#   1 NO                          ckpt.1
#
while read -r LINE ; do
	# Get dataset ID (first field) and checkpoint name (last field)
	ID=$(echo $LINE | awk '{print $1}')
	CKPT=$(echo $LINE | awk '{print $NF}')

	echo "Finalizing transfer for dataset $ID ($CKPT)" >> $LOGFILE
	if ! scr_flush_file -r -s $ID --dir $PREFIX &>> $LOGFILE ; then
		echo "Error: Can't resume dataset $ID, rc=$?" >> $LOGFILE
		continue
	fi

	echo "Writing summary for dataset $ID" >> $LOGFILE
	if ! scr_flush_file -s $ID -S --dir $PREFIX &>> $LOGFILE ; then
		echo "ERROR: Can't write summary for dataset $ID" >> $LOGFILE
		continue
	fi

	echo "Adding checkpoint $CKPT to index" >> $LOGFILE
	if ! scr_index -p $PREFIX --add=$CKPT &>> $LOGFILE ; then
		echo "Couldn't add checkpoint $CKPT to index" >> $LOGFILE
		continue
	fi

done < <(scr_index -l -p $PREFIX | grep NO)

echo "All done, index now:" >> $LOGFILE
scr_index -l -p $PREFIX &>> $LOGFILE
