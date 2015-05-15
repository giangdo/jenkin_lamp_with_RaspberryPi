#!/bin/sh

PIDFILE=/var/run/jenkin_mon.pid
GPIO_DIR=/sys/class/gpio
GPIO_MAX=39

gpio_init () {
	for i in `seq 0 $GPIO_MAX`; do
		if [ ! -L $GPIO_DIR/gpio$i ]; then
			# echo $i > $GPIO_DIR/export
			echo "Export"
		fi
		echo "set direction"
#		echo "out" > $GPIO_DIR/gpio$i/direction
#		echo "0" > $GPIO_DIR/gpio$i/value
	done
	return
}

echo $$ > $PIDFILE
gpio_init
while (true); do
# Polling tasks
	echo "Do polling tasks here."
	sleep 5
done

exit 0

#End of file
