# Copyright (C) Internet Systems Consortium, Inc. ("ISC")
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
# OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.

SYSTEMTESTTOP=..
. $SYSTEMTESTTOP/conf.sh

status=0
start=`date +%s`
end=`expr $start + 1200`
now=$start
while test $now -lt $end
do
	et=`expr $now - $start`
	echo "=============== $et ============"
	$JOURNALPRINT ns1/signing.test.db.signed.jnl | $PERL check_journal.pl
	$DIG axfr signing.test -p 5300 @10.53.0.1 > dig.out.at$et
	awk '$4 == "RRSIG" { print $11 }' dig.out.at$et | sort | uniq -c
	lines=`awk '$4 == "RRSIG" { print}' dig.out.at$et | wc -l`
	if [ ${et} -ne 0 -a ${lines} -ne 4009 ]
	then
		echo_i "failed"
		status=`expr $status + 1`
	fi
	sleep 20
	now=`date +%s`
done

echo_i "exit status: $status"
[ $status -eq 0 ] || exit 1
