#!/bin/sh
# with_timeout.sh SECONDS cmd args... — macOS has no coreutils `timeout`; perl's alarm does the job.
secs=$1; shift
exec perl -e 'my $s = shift; alarm $s; exec @ARGV or die "exec: $!"' "$secs" "$@"
