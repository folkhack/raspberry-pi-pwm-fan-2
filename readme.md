# Raspberry Pi PWM Fan Control in C and Python

* **CPU-efficient** - Does not rely on Python to run - is a C binary with 0% CPU utilization in top. Python 3 implementation only included as a POC.
* **Non-stepwise** - Avoids constant on/off cycling of fan by using different on/off temps, off grace periods, and easing if within min/max configured temps.
* **Configurable** - Uses environment variables for overriding default configuration.
* **systemd Service** - Setup as a persistent system service so that it runs on boot.
* **Raspberry Pi 3/4 Support** - 0% CPU utilization, tested on both Raspberry Pi 3 and 4 models.

---

## Wiring

* Default configuration is for a 5V Noctua PWM fan, but other PWM fans should support the same wiring
    - [NF-A4x10 5V PWM](https://noctua.at/en/nf-a4x10-5v-pwm) - Noctua 40x10mm fan
    - [NF-A4x20 5V PWM](https://noctua.at/en/nf-a4x20-5v-pwm) - Noctua 40x20mm fan
* "Pin #" are the board pin numbers; as-in "pin 1 starts at J8" numbers
* "GPIO #" are the BCM GPIO numbers used for software configuration via environment variables
* "Duty cycle signal" must be on a PWM capable GPIO pin:
    - Raspberry Pi 3/4 (channels share setting)
        + GPIO12 - PWM0
        + GPIO18 - PWM0
        + GPIO13 - PWM1
        + GPIO19 - PWM1
    - You may have to disable audio since it requires PWM to work via `dtparam=audio=off` in `/boot/config.txt`

|Pin #|GPIO #|Name|Color|Notes|
|---|---|---|---|---|
|4||**5V**|Yellow||
|6||**Ground**|Black||
|12|18|**Duty cycle signal**|Blue|PWM channel #0|
|18|24|**Tachometer signal**|Green|(optional) Any GPIO pin should work|
|17||**Tachometer pull-up**|Orange|(optional) bridge to "tachometer signal" with a 1k Ω resistor; not a wire from fan|

![Raspberry Pi PWM fan wiring diagram](docs/rpi_fan_diagram.png "Raspberry Pi PWM fan wiring diagram")

For a more detailed explaination of the wiring visit DriftKingTW's blog post ["Using Raspberry Pi to Control a PWM Fan and Monitor its Speed."](https://blog.driftking.tw/en/2019/11/Using-Raspberry-Pi-to-Control-a-PWM-Fan-and-Monitor-its-Speed/#Wiring) These scripts are based on their fantastic work.

---

## Scripted `install.sh` Install

**IMPORTANT!:** The service does not start by default because it expects you to configure the GPIO pin and PWM fan duty cycle hertz to your fan's specification.

**[See: Environment Variables Config &raquo;](#environment-variables)**

```bash
# From the Raspberry Pi (builds for installed OS variant)
sudo install.sh

# Running with defaults:
# - IMPORTANT! Only for Noctua 5V PWM fans!
sudo ./pwm_fan_control2

# Display help and exit:
#   sudo ./pwm_fan_control2 --help

# For testing other fans configurations override environment variables by CLI with sudo:
#   export PWM_FAN_SLEEP_S=5 && \
#   export PWM_FAN_MIN_DUTY_CYCLE=40 && \
#   sudo -E bash -c './pwm_fan_control2'

# Configure the service (PWM_FAN_PWM_FREQ_HZ, etc. - see readme.md):
# - Not required for Noctua 5V PWM fans
sudo nano /etc/systemd/system/pwm_fan_control2.service
sudo systemctl daemon-reload

# Start the service:
sudo service pwm_fan_control2 start

# Enable service at boot:
sudo systemctl enable pwm_fan_control2.service

# Misc control:
#   sudo service pwm_fan_control2 status
#   sudo service pwm_fan_control2 stop
#   sudo service pwm_fan_control2 restart

# Uninstall - stops and removes installed system service + binary:
#   sudo make uninstall
```

---


## Manual Build and Install

**Requirements:**

* Git and basic build tooling are required
* `wiringPi.h` is required from https://github.com/WiringPi/WiringPi
* `build` script is in root of WiringPi project

```bash
# Install build tooling
sudo apt install git build-essential

# Obtain and install up-to-date fork of WiringPi
mkdir ~/install && cd ~/install
git clone https://github.com/WiringPi/WiringPi.git
cd WiringPi
sudo ./build

# Get this project
git clone https://github.com/folkhack/raspberry-pi-pwm-fan-2.git
cd raspberry-pi-pwm-fan
```

**Building:**

* Building/installing are done through a simple makefile
* `compile-debug` mode has CLI output with current CPU temp, mode of operation, fan duty cycle, and fan RPMs (if configured) for testing new fan configurations

```bash
# From the Raspberry Pi (builds for installed OS variant)
make compile

# Compile with CLI debugging enabled - NOT FOR PRODUCTION USE!!
#   make compile-debug
```

**Running:**

Must be ran with root permissions!

```bash
sudo ./pwm_fan_control2

# Display help and exit:
#   sudo ./pwm_fan_control2 --help

# Override environment variables by CLI with sudo:
#   export PWM_FAN_SLEEP_S=5 && \
#   export PWM_FAN_MIN_DUTY_CYCLE=40 && \
#   sudo -E bash -c './pwm_fan_control2'

# Use a .env configuration as sudo (sample .env included .env.sample):
#   source .env && sudo -E bash -c './pwm_fan_control2'

# Unset previously set environment variables:
#   unset PWM_FAN_SLEEP_S
#   unset PWM_FAN_MIN_DUTY_CYCLE
```

**Installing as system service:**

**IMPORTANT!:** The service does not start by default because it expects you to configure the GPIO pin and PWM fan duty cycle hertz to your fan's specification.

**[See: Environment Variables Config &raquo;](#environment-variables)**

```bash
# Install:
sudo make install

# Uninstall - stops and removes installed system service + binary:
#   sudo make uninstall

# Configure the service (PWM_FAN_PWM_FREQ_HZ, etc. - see readme.md):
sudo nano /etc/systemd/system/pwm_fan_control2.service
sudo systemctl daemon-reload

# Start the service:
sudo service pwm_fan_control2 start

# Enable service at boot:
sudo systemctl enable pwm_fan_control2.service
```

---

## Environment Variables

After installation you can edit the service file:

```bash
# Edit system service:
sudo nano /etc/systemd/system/pwm_fan_control2.service

# Reload systemd and restart service:
sudo systemctl daemon-reload
sudo service pwm_fan_control2 restart
```

#### systemd configuration environment variables in service:

Useful for setting a few configuration values.

```ini
[Unit]
Description=PWM fan speed control
After=network.target

[Service]
Environment=PWM_FAN_SLEEP_S=5
Environment=PWM_FAN_MIN_DUTY_CYCLE=40
ExecStart=/usr/sbin/pwm_fan_control2
Type=simple
User=root
Group=root
Restart=always

[Install]
WantedBy=multi-user.target
```

#### systemd configuration using a config file:

* Useful for large amounts of configuration values
* Sample configuration/template provided with `.env.sample`
* Following example uses `.env.sample` file at `/home/pi/install/raspberry-pi-pwm-fan-2/.env.sample`
* Recommended to move this to `/etc` and rename it for your own needs

```ini
[Unit]
Description=PWM fan speed control
After=network.target

[Service]
EnvironmentFile=/home/pi/install/raspberry-pi-pwm-fan-2/.env.sample
ExecStart=/usr/sbin/pwm_fan_control2
Type=simple
User=root
Group=root
Restart=always

[Install]
WantedBy=multi-user.target
```

### Environment Varibale Reference:

**NOTE:** These environment variables are fully supported by *both* the Python 3 POC and the formal C implementation!

|Variable|Default|Type|Notes|
|---|---|---|---|
|**`PWM_FAN_BCM_GPIO_PIN_PWM`**|18|unsigned short|BCM GPIO pin for PWM duty cycle signal|
|**`PWM_FAN_BCM_GPIO_PIN_TACH`**|24|unsigned short|(optional) - BCM GPIO pin for PWM tachometer signal; 0 to disable tachometer support|
|**`PWM_FAN_PWM_FREQ_HZ`**|2500|unsigned short|PWM duty cycle target freqency Hz - from Noctua Spec at 25kHz|
|**`PWM_FAN_MIN_DUTY_CYCLE`**|20|unsigned short|Minimum PWM duty cycle - from Noctua spec at 20%|
|**`PWM_FAN_MAX_DUTY_CYCLE`**|100|unsigned short|Maximum PWM duty cycle|
|**`PWM_FAN_MIN_OFF_TEMP_C`**|38|float|Turn fan off if is on and CPU temp falls below this value|
|**`PWM_FAN_MIN_ON_TEMP_C`**|40|float|Turn fan on if is off and CPU temp rises above this value|
|**`PWM_FAN_MAX_TEMP_C`**|46|float|Set fan duty cycle to `PWM_FAN_MAX_DUTY_CYCLE` if CPU temp rises above this value|
|**`PWM_FAN_FAN_OFF_GRACE_S`**|60|unsigned short|Turn fan off if CPU temp stays below `MIN_OFF_TEMP_C` this for time period|
|**`PWM_FAN_SLEEP_S`**|1|unsigned short|Main loop check CPU and set PWM duty cycle delay|
|**`PWM_FAN_TACH_PULSE_PER_REV`**|2|unsigned short|from Noctua spec at 2 pulses per revolution|
|**`PWM_FAN_TACH_MIN_TIME_DELTA_S`**|0.005|float|Tachometer event time delta cutoff to avoid spuriously short pulses|

---

## Python POC - `./pwm_fan_control_poc.py`

Included is a Python 3 implementation that supports colored stdout output and writes CPU temp, PWM duty cycle, and tachometer values to a CSV file.

##### Python POC Quickstart:

**Setup:**

```bash
# For colored terminal output:
sudo apt install python3-termcolor
```
**Running:**

* Must be ran with root permissions
* Will write a CSV file ex: test_1667157372.3389149.csv with CPU temp, fan duty cycle, and fan speed

```bash
sudo python3 ./pwm_fan_control_poc.py

# Override environment variables by CLI with sudo
#   export PWM_FAN_SLEEP_S=5 && \
#   export PWM_FAN_MIN_DUTY_CYCLE=40 && \
#   sudo -E bash -c 'python3 pwm_fan_control_poc.py'

# Use a .env configuration as sudo (sample .env included .env.sample)
#   source .env && sudo -E bash -c 'python3 pwm_fan_control_poc.py'
```

---

## Notes/Misc

#### Why C over Python:

When running the Python POC at full 25khz PWM frequency (Noctua Spec) CPU consumption can be upwards of 5-10%. With C and WiringPi it's at 0% on a Raspberry 4.

#### WiringPi instead of pigpio:

I did try to port this over to the better-supported pigpio library. Unfortunately it resulted in consistent 5-10% CPU use on a Raspberry Pi 4 which was unacceptable.

#### Easing Function:

A quartic bezier easing function was used to smooth fan speed at the upper/lower boundries of the configured temps `PWM_FAN_MIN_OFF_TEMP_C` and `PWM_FAN_MAX_TEMP_C`. At temps closer to the lower boundry, the fan speed is kept close to the `PWM_FAN_MIN_DUTY_CYCLE`, and at the higher boundry fan speed will stay closer to `PWM_FAN_MAX_DUTY_CYCLE`.

* Raspberry Pi PWM Fan Linear & Quartic Bezier Fan Easing Graphed:
https://docs.google.com/spreadsheets/d/135dJXuy5qX0IenmxIjSwHkgeXwgmW6yCtiCEznN_yzk
* Quartic bezier ease-in/out function
https://easings.net/#easeInOutQuart
* Desmos quartic bezier function graphed
https://www.desmos.com/calculator/d5f3jhma63
* Raspberry PI PWM Fan Test CSV Graphed:
https://docs.google.com/spreadsheets/d/1FNpywV0M-U6qUqJ1sFyiU5g77cSyhXuzXnMXVCM04ls

#### Links:

* Noctua A4X20 5V PWM Fan Spec:
https://noctua.at/en/nf-a4x20-pwm/specification
* Noctua General PWM Fan Spec:
https://noctua.at/pub/media/wysiwyg/Noctua_PWM_specifications_white_paper.pdf
* DriftKingTw's Blog Python implementation:
https://blog.driftking.tw/en/2019/11/Using-Raspberry-Pi-to-Control-a-PWM-Fan-and-Monitor-its-Speed/
* DIY Life PWM Article:
https://www.the-diy-life.com/connecting-a-pwm-fan-to-a-raspberry-pi/
* mklements PWMFanControl Python implementation (from above DIY Life article):
https://github.com/mklements/PWMFanControl
* Raspberry Pi PWM Generation using Python and C:
https://www.electronicwings.com/raspberry-pi/raspberry-pi-pwm-generation-using-python-and-c
