```bash
# To get base model name
cat /sys/firmware/devicetree/base/model

# To watch the PWM state
sudo cat /sys/kernel/debug/pwm

# To get the GPIO mappings
cat /sys/kernel/debug/gpio

# "Raspberry Pi 5 Model B"
#     /sys/class/pwm/pwmchip2
#       BCM GPIO 12 - channel 0
#       BCM GPIO 13 - channel 1
#       BCM GPIO 18 - channel 2
#       BCM GPIO 19 - channel 3
#   GPIO mapping:
#     GPIO12 - gpio-583
#     GPIO13 - gpio-584
#     GPIO18 - gpio-589
#     GPIO19 - gpio-590

# "Raspberry Pi 4 Model B"
#     /sys/class/pwm/pwmchip0
#       BCM GPIO 12 - channel 0
#       BCM GPIO 13 - channel 1
#       BCM GPIO 18 - channel 0
#       BCM GPIO 19 - channel 1
#   GPIO mapping:
#     GPIO12 - gpio-524
#     GPIO13 - gpio-525
#     GPIO18 - gpio-530
#     GPIO19 - gpio-531

# "Raspberry Pi 3 Model B Plus"
#   /sys/class/pwm/pwmchip0
#     BCM GPIO 12 - channel 0
#     BCM GPIO 13 - channel 1
#     BCM GPIO 18 - channel 0
#     BCM GPIO 19 - channel 1
#   GPIO mapping:
#     GPIO12 - gpio-524
#     GPIO13 - gpio-525
#     GPIO18 - gpio-530
#     GPIO19 - gpio-531
```