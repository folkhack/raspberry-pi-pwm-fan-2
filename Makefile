OPTS   = -g -O0 -Wall
ENTRY  = main.c
LIBS   = -L /usr/local/include -lm -lrt -lpthread
TARGET = pwm_fan_control2

# COMPILE:
compile:
	gcc ${OPTS} ${ENTRY} ${LIBS} -o ${TARGET}
	chmod +x ${TARGET}

clean:
	rm ${TARGET}

# INSTALL/UNINSTALL:
install:
	cp ${TARGET} /usr/sbin/${TARGET}
	chown root:root /usr/sbin/${TARGET}
	cp ${TARGET}.service /etc/systemd/system/${TARGET}.service

uninstall:
	systemctl -f stop ${TARGET}.service
	systemctl disable ${TARGET}.service
	systemctl daemon-reload

	rm -f /etc/systemd/system/${TARGET}.service
	rm -f /usr/sbin/${TARGET}
