#!/bin/bash

echo "Starting pwm_fan_control2 installation script..."

if [ "$( id -u )" != "0" ]; then
    echo "This script must be run as root" 1>&2
    exit 1
fi

echo "Checking for git and build-essential dependencies..."

if ! dpkg -s git build-essential >/dev/null 2>&1; then

    echo "Ensuring git and build-essential installed..."
    apt update -y
    apt install -y git build-essential

else

    echo "git and build-essential already installed!"
fi

echo "Installing pwm_fan_control2..."
echo "Building, installing, and starting pwm_fan_control2..."

# Build and install pwm_fan_control2
make compile
make install

echo "Fixing binary owner..."

# Fix ownership of binary
chown $SUDO_USER:$SUDO_USER pwm_fan_control2

echo "Done! pwm_fan_control2 installed as persistent system service!"
echo ""
echo "  IMPORTANT! Next steps:"
echo ""
echo "  Configure the service (PWM_FAN_PWM_FREQ_HZ, etc. - see readme.md):"
echo "     sudo nano /etc/systemd/system/pwm_fan_control2.service"
echo "     systemctl daemon-reload"
echo ""
echo "  Start the service:"
echo "     service pwm_fan_control2 start"
echo ""
echo "  Enable service at boot:"
echo "     systemctl enable pwm_fan_control2.service"
echo ""
echo "  Misc:"
echo "     service pwm_fan_control2 status"
echo "     service pwm_fan_control2 stop"
echo "     service pwm_fan_control2 restart"
echo ""
