import RPi.GPIO as GPIO
import time
import subprocess
import os

# sudo apt install python3-termcolor
from termcolor import colored

####################################################################################################
#
#  Config
#  - See readme.md for documentation
#

BCM_GPIO_PIN_PWM      = int(   os.environ.get( 'PWM_FAN_BCM_GPIO_PIN_PWM',      18 ) )
BCM_GPIO_PIN_TACH     = int(   os.environ.get( 'PWM_FAN_BCM_GPIO_PIN_TACH',     24 ) )
PWM_FREQ_HZ           = int(   os.environ.get( 'PWM_FAN_PWM_FREQ_HZ',           2500 ) )
MIN_DUTY_CYCLE        = int(   os.environ.get( 'PWM_FAN_MIN_DUTY_CYCLE',        20 ) )
MAX_DUTY_CYCLE        = int(   os.environ.get( 'PWM_FAN_MAX_DUTY_CYCLE',        100 ) )
MIN_OFF_TEMP_C        = float( os.environ.get( 'PWM_FAN_MIN_OFF_TEMP_C',        38 ) )
MIN_ON_TEMP_C         = float( os.environ.get( 'PWM_FAN_MIN_ON_TEMP_C',         40 ) )
MAX_TEMP_C            = float( os.environ.get( 'PWM_FAN_MAX_TEMP_C',            46 ) )
FAN_OFF_GRACE_S       = int(   os.environ.get( 'PWM_FAN_FAN_OFF_GRACE_S',       60 ) )
SLEEP_S               = int(   os.environ.get( 'PWM_FAN_SLEEP_S',               1 ) )
TACH_MIN_TIME_DELTA_S = float( os.environ.get( 'PWM_FAN_TACH_MIN_TIME_DELTA_S', 0.005 ) )
TACH_PULSE_PER_REV    = int(   os.environ.get( 'PWM_FAN_TACH_PULSE_PER_REV',    2 ) )


####################################################################################################
#
#  Runtime setup
#

# Track if everything is setup to tell if writing to the PWM is safe
is_setup = False

print( "Starting Raspberry Pi PWM fan controller..." )
print( "Starting PWM fan control service on GPIO pin #" + str( BCM_GPIO_PIN_PWM ) + "... <3 folkhack 2022" )

# Setup BCM output pin
GPIO.setwarnings( False )
GPIO.setmode( GPIO.BCM )
GPIO.setup( BCM_GPIO_PIN_PWM, GPIO.OUT )

# Setup fan PWM
pwm_fan = GPIO.PWM( BCM_GPIO_PIN_PWM, PWM_FREQ_HZ )

print( "GPIO initialized! GPIO #" + str( BCM_GPIO_PIN_PWM ) + " set to output + PWM Hz set to " + str( PWM_FREQ_HZ ) + "Hz!" )

# Setup tach pin - pull up to 3.3V
if BCM_GPIO_PIN_TACH > 0:

    print( "Tachometer support enabled on GPIO #" + str( BCM_GPIO_PIN_TACH ) + "! Setting up pin for input..." )

    GPIO.setup( BCM_GPIO_PIN_TACH, GPIO.IN, pull_up_down = GPIO.PUD_UP )

    print( "Tachometer input setup!" )

# PWM ready for duty cycle set
is_setup = True

print( "Blipping to full duty cycle " + str( MAX_DUTY_CYCLE ) + " for 10s..." )

# Blip fan to max duty cycle for 2s before start
pwm_fan.start( MAX_DUTY_CYCLE )
time.sleep( 10 )

print( "10s fan blip finished!" )

# Track last fan enabled state and when the last time we were above the minimum threshold for
#    smoothing on/off cycles
last_fan_enabled     = False
last_above_min_epoch = time.time()

# Track tachometer
tach_rpm             = 0
tach_last_fall_epoch = time.time()

####################################################################################################
#
#  Functions
#

#
#   Get CPU temp as float
#
def get_cpu_temp_c():

    try:
        output   = subprocess.run( [ 'vcgencmd', 'measure_temp' ], capture_output = True )
        temp_str = output.stdout.decode()

        return float( temp_str.split( '=' )[1].split( '\'' )[0] )

    except( IndexError, ValueError ):
        raise RuntimeError( 'Could not get temperature' )

#
#   Quartic bezier easing function
#   - https://easings.net/#easeInOutQuart
#   - ^ = ** in Python
#
def quartic_bezier_easing( cur_val, range_1_low, range_1_high, range_2_low, range_2_high ):

    range_1_delta                    = range_1_high - range_1_low
    range_2_delta                    = range_2_high - range_2_low
    pct_range_1_delta                = 1 - ( ( range_1_high - cur_val ) / range_1_delta )
    pct_quartic_bezier_range_2_delta = 8 * pct_range_1_delta ** 4 if pct_range_1_delta < 0.5 else 1 - ( ( -2 * pct_range_1_delta + 2 ) ** 4 ) / 2
    quartic_bezier_val               = pct_quartic_bezier_range_2_delta * range_2_delta + range_2_low

    return int( quartic_bezier_val )

#
#   Off function - GPIO clean-up & set PWM to duty cycle max for safety
#
def handle_halt():

    print( "SIGINT detected!" )

    if is_setup:

        print( "WiringPi PWM setup - setting to MAX_DUTY_CYCLE " + str( MAX_DUTY_CYCLE ) + " and cleaning-up GPIO before exit..." )
        pwm_fan.ChangeDutyCycle( MAX_DUTY_CYCLE )
        GPIO.cleanup()

    print( "Exiting with status 0..." )

    exit( 0 )

#
#   Tachometer pull-down event handler - sets RPM tracking globals
#
def on_tach_pull_down( whatever ):

    global TACH_PULSE_PER_REV
    global TACH_MIN_TIME_DELTA_S
    global tach_last_fall_epoch
    global tach_rpm

    delta_time = time.time() - tach_last_fall_epoch

    # Reject spuriously short pulses
    if delta_time < TACH_MIN_TIME_DELTA_S: return

    tach_rpm             = int( ( 1 / delta_time ) / TACH_PULSE_PER_REV ) * SLEEP_S * 60
    tach_last_fall_epoch = time.time()


####################################################################################################
#
#  Setup event handlers and main loop
#

csv_file = 'test_' + str( time.time() ) + '.csv'

print( "Starting test time,temp,duty_cycle,tach_rpm CSV \"" + csv_file + "\"..." )

f = open( csv_file, 'a+' )

f.write( 'time,temp,duty_cycle' )

# Setup tachometer if needed
if BCM_GPIO_PIN_TACH > 0:

    print( "Setting up tachometer pull-down event handler..." )

    # Add the tach pull-down event handler
    GPIO.add_event_detect( BCM_GPIO_PIN_TACH, GPIO.FALLING, on_tach_pull_down )

    print( "Tachometer pull-down event handler setup!" )

    # Add a column to the reporting CSV for tach speed
    f.write( ',tach_rpm' )

f.write( '\n' )

print( "Test CSV \"" + csv_file + "\" ready!" )

try:

    print( "Starting CPU temp polling at " + str( SLEEP_S ) + "s sleep interval..." )

    while 1:

        cur_temp_c         = get_cpu_temp_c()
        duty_cycle_set_val = 0

        # If fan is on, then we use MIN_OFF_TEMP_C else we use MIN_ON_TEMP_C to smooth on/off cycles
        use_min_temp_c = MIN_OFF_TEMP_C if last_fan_enabled == True else MIN_ON_TEMP_C

        # If we're above min off temp then set last_above_min_epoch
        if cur_temp_c > use_min_temp_c:
            last_above_min_epoch = time.time()

        # If we're below min temp and within fan off grace period set to min duty cycle
        if cur_temp_c <= use_min_temp_c and time.time() - last_above_min_epoch < FAN_OFF_GRACE_S:
            duty_cycle_set_val = MIN_DUTY_CYCLE

            print( colored( str( cur_temp_c ), 'cyan' ) + " BELOW     use_min_temp_c - MIN_DUTY_CYCLE   ", end = '' )

        # If we're below min temp and outside of fan grace period set to 0 duty cycle (off)
        elif cur_temp_c <= use_min_temp_c:
            duty_cycle_set_val = 0

            print( colored( str( cur_temp_c ), 'green' ) + " BELOW_OFF use_min_temp_c - OFF              ", end = '' )

        # If current temp is greater than max temp set to max duty cycle
        elif cur_temp_c >= MAX_TEMP_C:
            duty_cycle_set_val = MAX_DUTY_CYCLE

            print( colored( str( cur_temp_c ), 'red' ) + " ABOVE     MAX_TEMP_C - MAX_DUTY_CYCLE       ", end = '' )

        # If current temp is within min/max temp range use normalized value
        # - Use min_off_temp to keep fan curve consistent (will yeild a faster fan speed vs. using min_on_temp)
        else:
            duty_cycle_set_val = quartic_bezier_easing( cur_temp_c, MIN_OFF_TEMP_C, MAX_TEMP_C, MIN_DUTY_CYCLE, MAX_DUTY_CYCLE )

            print( colored( str( cur_temp_c ), 'yellow' ) + " ABOVE     MAX_TEMP_C - quartic_bezier_easing", end = '' )

        # Track last fan enabled state
        last_fan_enabled = True if duty_cycle_set_val > 0 else False

        pwm_fan.ChangeDutyCycle( duty_cycle_set_val )

        print( ' - Duty cycle = ' + colored( str( duty_cycle_set_val ), 'magenta' ), end = '' )
        f.write( str( round( time.time() ) ) + ',' + str( cur_temp_c ) + ',' + str( duty_cycle_set_val ) )

        # Handle tach if needed
        if BCM_GPIO_PIN_TACH > 0:

            print( ' - Tach RPM = ' + colored( str( tach_rpm ), 'cyan' ), end = '' )
            f.write( ',' + str( tach_rpm ) )

            tach_rpm = 0

        print()
        f.write( '\n' )
        f.flush()

        time.sleep( SLEEP_S )

# Handle SIGINT
except KeyboardInterrupt:
    handle_halt()
