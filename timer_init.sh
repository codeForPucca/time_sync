#!/bin/sh
### BEGIN INIT INFO
# Provides:          meinprogramm
# Required-Start:    $remote_fs $network
# Required-Stop:     $remote_fs $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Startet mein Programm beim Booten
# Description:       Ein Init-Skript zum Start eines benutzerdefinierten Programms
### END INIT INFO


DAEMON=/home/vio/time_sync/slave
NAME=time_syncer
PIDFILE=/var/run/$NAME.pid

start() {
    echo "Start $NAME..."
    start-stop-daemon --start --background --make-pidfile --pidfile $PIDFILE --exec $DAEMON
}

stop() {
    echo "Stoppe $NAME..."
    start-stop-daemon --stop --pidfile $PIDFILE
}

case "$1" in
    start)
        start
        ;;
    stop)
        stop
        ;;
    restart)
        stop
        sleep 1
        start
        ;;
    *)
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac

exit 0
