#!/bin/bash

echo "Installing pwm_fan_control2..."

###

echo "Installing git/build-essential..."

# Install build tooling + get for dependencies
apt update -y
apt install -y git build-essential

echo "Building and installing wiringPi..."

# Install WiringPi
pushd /tmp

git clone https://github.com/WiringPi/WiringPi.git
pushd WiringPi
sudo ./build

popd
popd

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
