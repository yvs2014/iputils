#!/bin/sh
# Copyright (c) Iputils Project, 2018-2024
# Meson install script to setcap or setuid to an executable.

exec_path="$1/$2"
perm_type="$3"
setcap="$4"

if [ -n "$DESTDIR" ]; then
	exec_path="${DESTDIR%/}/${exec_path}"
fi

_log() {
	echo "$(basename $0): $1"
}

case "$perm_type" in
	caps)
		params="cap_net_raw+p"
		[ "$2" = "clockdiff" ] && params="cap_net_raw,cap_sys_nice+p"
		# cap_net_admin is needed for ping -m even on ICMP datagram socket
		# (or cap_net_raw since Linux kernel 5.17)
		# [ "$2" = "ping" ] && params="cap_net_admin,cap_net_raw+p"
		_log "calling: $setcap $params $exec_path"
		"$setcap" $params "$exec_path"
	;;

	suid)
		_log "changing '$exec_path' to be setuid root executable"
		chown -v root "$exec_path"
		chmod -v u+s "$exec_path"
	;;

	*)
		_log "unexpected argument: '$perm_type'"
		exit 1
	;;
esac

exit 0
