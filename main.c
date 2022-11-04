#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiringPi.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <math.h>
#include <signal.h>

#ifdef DEBUG

    // gettimeofday for tachometer support
    #include <sys/time.h>

    // Colored output
    #define RED     "\x1b[31m"
    #define GREEN   "\x1b[32m"
    #define YELLOW  "\x1b[33m"
    #define MAGENTA "\x1b[35m"
    #define CYAN    "\x1b[36m"
    #define RESET   "\x1b[0m"

#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Config - default vals
//  - See readme.md for documentation
//

// Declare configuration variables w/expected type
unsigned short BCM_GPIO_PIN_PWM      = 18,
               PWM_FREQ_HZ           = 2500,
               MIN_DUTY_CYCLE        = 20,
               MAX_DUTY_CYCLE        = 100,
               FAN_OFF_GRACE_S       = 60,
               SLEEP_S               = 1,
               BCM_GPIO_PIN_TACH     = 24,
               TACH_PULSE_PER_REV    = 2;

float          MIN_OFF_TEMP_C        = 38,
               MIN_ON_TEMP_C         = 40,
               MAX_TEMP_C            = 46,
               TACH_MIN_TIME_DELTA_S = 0.005;

// Is setup flag to know if writing to the PWM is safe
bool is_setup = false;

// Keep WiringPi pin in a broad scope to set to full duty cycle on SIGINT
unsigned short wiringpi_pin_pwm = 0;

// Track tachometer (only enabled during debug)
#ifdef DEBUG

    unsigned short wiringpi_pin_tach = 0,
                   tach_rpm          = 0;

    struct timeval tach_last_fall_epoch;

#endif

////////////////////////////////////////////////////////////////////////////////
//
//  Functions
//

//
//  Lookup a WiringPi pin # from the BCM GPIO pin #
//  - Works with Raspberry Pi 3 & 4
//
unsigned short get_wiringpi_pin_from_bcm_gpio( unsigned short gpio_pin ) {

    static const unsigned short wiring_pi_bcm_gpio_lookup[] = { 
        [0]  = 30,
        [1]  = 31,
        [2]  = 8,
        [3]  = 9,
        [4]  = 7,
        [5]  = 21,
        [6]  = 22,
        [7]  = 11,
        [8]  = 10,
        [9]  = 13,
        [10] = 12,
        [11] = 14,
        [12] = 26,
        [13] = 23,
        [14] = 15,
        [15] = 16,
        [16] = 27,
        [17] = 0,
        [18] = 1,
        [19] = 24,
        [20] = 28,
        [21] = 29,
        [22] = 3,
        [23] = 4,
        [24] = 5,
        [25] = 6,
        [26] = 25,
        [27] = 2
    };

    return wiring_pi_bcm_gpio_lookup[ gpio_pin ];
}

//
//  Get CPU temp as float
//
float get_cpu_temp_c() {

    // Value in "temp" file is degrees in C * 1000
    float cpu_temp_raw;

    // Setup the CPU temp file
    FILE * cpu_temp_file;

    // `/sys/class/thermal/thermal_zone0/temp` on Raspberry Pi contains current temp in Celsius * 1000
    cpu_temp_file = fopen( "/sys/class/thermal/thermal_zone0/temp", "r" );

    // If CPU temp can't be read return -1
    if( cpu_temp_file == NULL ) {

        return -1;
    }

    // Read the temp into the `cpu_temp_raw` variable by reference
    fscanf( cpu_temp_file, "%f", &cpu_temp_raw );
    fclose( cpu_temp_file );

    // Convert to correct Celsius temp double
    return cpu_temp_raw / 1000;
}

//
//  Quartic bezier easing function
//  - https://easings.net/#easeInOutQuart
//
unsigned short quartic_bezier_easing(
    float cur_val,
    unsigned short range_1_low,
    unsigned short range_1_high,
    unsigned short range_2_low,
    unsigned short range_2_high ) {

    unsigned short range_1_delta = range_1_high - range_1_low,
                   range_2_delta = range_2_high - range_2_low;

    float pct_range_1_delta = 1 - ( ( range_1_high - cur_val ) / range_1_delta );
    float pct_quartic_bezier_range_2_delta;

    if( pct_range_1_delta < 0.5 ) {

        pct_quartic_bezier_range_2_delta = 8 * pow( pct_range_1_delta, 4 );

    } else {

        pct_quartic_bezier_range_2_delta = 1 - ( pow( -2 * pct_range_1_delta + 2, 4 ) ) / 2;
    }

    float quartic_bezier_val = pct_quartic_bezier_range_2_delta * range_2_delta + range_2_low;

    return (unsigned short) round( quartic_bezier_val );
}

//
//  SIGINT/SIGTERM handler
//
void handle_halt( int noop ) {

    printf( "SIGINT detected!\n" );

    if( is_setup && wiringpi_pin_pwm ) {

        printf( "WiringPi PWM setup - setting to MAX_DUTY_CYCLE %i before exit...\n", MAX_DUTY_CYCLE );
        pwmWrite( wiringpi_pin_pwm, MAX_DUTY_CYCLE );
    }

    printf( "Exiting with status 0...\n" );

    exit( 0 );
}

//
//  Tach pull-down event
//  - Only available if debugging
//
#ifdef DEBUG

    void on_tach_pull_down() {

        struct timeval cur_epoch;
        gettimeofday( &cur_epoch, NULL );

        float delta_time = ( cur_epoch.tv_usec - tach_last_fall_epoch.tv_usec ) * 0.000001;

        gettimeofday( &tach_last_fall_epoch, NULL );

        // Reject spuriously short pulses
        if( delta_time < TACH_MIN_TIME_DELTA_S ) return;

        tach_rpm = round( ( 1 / delta_time ) / TACH_PULSE_PER_REV ) * SLEEP_S * 60;
    }

#endif

////////////////////////////////////////////////////////////////////////////////////

int main( int argc, char* argv[] ) {

    // Register SIGINT/SIGTERM handler
    signal( SIGINT, handle_halt );
    signal( SIGTERM, handle_halt );

    // Disable stdout buffering so logs show up in journal
    setbuf( stdout, NULL );

    ////////////////////////////////////////////////////////////////////////////////
    //
    //  Help CLI
    //

    // Check for --help argument
    if( argc > 1 && strcmp( argv[1], "--help" ) == 0 ) {

        printf( "\nRaspberry Pi CPU PWM Fan Controller v2 \n"
                "\n"
                "Usage: ./pwm_fan_control2 \n"
                "\n"
                " - Watches CPU temp and sets PWM fan speed accordingly with WiringPi library.\n"
                " - Configured through environment variables; see readme.md for documentation.\n"
                "\n"
                "Exit status:\n"
                "  0 if OK\n"
                "  1 if error\n"
                "\n"
                "Online help, docs & bug reports: <https://github.com/folkhack/raspberry-pi-pwm-fan-2> \n"
        );

        return 0;
    }

    printf( "Starting Raspberry Pi PWM fan controller...\n" );

    ////////////////////////////////////////////////////////////////////////////////
    //
    //  Config - set from environment variables
    //  - See readme.md for documentation
    //

    if( getenv( "PWM_FAN_BCM_GPIO_PIN_PWM" ) )      sscanf( getenv( "PWM_FAN_BCM_GPIO_PIN_PWM" ),      "%hu", &BCM_GPIO_PIN_PWM );;
    if( getenv( "PWM_FAN_PWM_FREQ_HZ" ) )           sscanf( getenv( "PWM_FAN_PWM_FREQ_HZ" ),           "%hu", &PWM_FREQ_HZ );
    if( getenv( "PWM_FAN_MIN_DUTY_CYCLE" ) )        sscanf( getenv( "PWM_FAN_MIN_DUTY_CYCLE" ),        "%hu", &MIN_DUTY_CYCLE );
    if( getenv( "PWM_FAN_MAX_DUTY_CYCLE" ) )        sscanf( getenv( "PWM_FAN_MAX_DUTY_CYCLE" ),        "%hu", &MAX_DUTY_CYCLE );
    if( getenv( "PWM_FAN_FAN_OFF_GRACE_S" ) )       sscanf( getenv( "PWM_FAN_FAN_OFF_GRACE_S" ),       "%hu", &FAN_OFF_GRACE_S );
    if( getenv( "PWM_FAN_SLEEP_S" ) )               sscanf( getenv( "PWM_FAN_SLEEP_S" ),               "%hu", &SLEEP_S );
    if( getenv( "PWM_FAN_MIN_OFF_TEMP_C" ) )        sscanf( getenv( "PWM_FAN_MIN_OFF_TEMP_C" ),        "%f",  &MIN_OFF_TEMP_C );
    if( getenv( "PWM_FAN_MIN_ON_TEMP_C" ) )         sscanf( getenv( "PWM_FAN_MIN_ON_TEMP_C" ),         "%f",  &MIN_ON_TEMP_C );
    if( getenv( "PWM_FAN_MAX_TEMP_C" ) )            sscanf( getenv( "PWM_FAN_MAX_TEMP_C" ),            "%f",  &MAX_TEMP_C );
    if( getenv( "PWM_FAN_BCM_GPIO_PIN_TACH" ) )     sscanf( getenv( "PWM_FAN_BCM_GPIO_PIN_TACH" ),     "%hu", &BCM_GPIO_PIN_TACH );
    if( getenv( "PWM_FAN_TACH_MIN_TIME_DELTA_S" ) ) sscanf( getenv( "PWM_FAN_TACH_MIN_TIME_DELTA_S" ), "%f",  &TACH_MIN_TIME_DELTA_S );
    if( getenv( "PWM_FAN_TACH_PULSE_PER_REV" ) )    sscanf( getenv( "PWM_FAN_TACH_PULSE_PER_REV" ),    "%hu", &TACH_PULSE_PER_REV );

    #ifdef DEBUG

        printf( "\nConfig:\n" );
        printf( " - BCM_GPIO_PIN_PWM      = %i\n", BCM_GPIO_PIN_PWM );
        printf( " - PWM_FREQ_HZ           = %i\n", PWM_FREQ_HZ );
        printf( " - MIN_DUTY_CYCLE        = %i\n", MIN_DUTY_CYCLE );
        printf( " - MAX_DUTY_CYCLE        = %i\n", MAX_DUTY_CYCLE );
        printf( " - MIN_OFF_TEMP_C        = %f\n", MIN_OFF_TEMP_C );
        printf( " - MIN_ON_TEMP_C         = %f\n", MIN_ON_TEMP_C );
        printf( " - MAX_TEMP_C            = %f\n", MAX_TEMP_C );
        printf( " - FAN_OFF_GRACE_S       = %i\n", FAN_OFF_GRACE_S );
        printf( " - SLEEP_S               = %i\n", SLEEP_S );
        printf( " - BCM_GPIO_PIN_TACH     = %i\n", BCM_GPIO_PIN_TACH );
        printf( " - TACH_MIN_TIME_DELTA_S = %f\n", TACH_MIN_TIME_DELTA_S );
        printf( " - TACH_PULSE_PER_REV    = %i\n", TACH_PULSE_PER_REV );

    #endif

    ////////////////////////////////////////////////////////////////////////////////
    //
    //  Runtime setup
    //

    // PWM values
    // - PWM frequency in Hz = 19.2e6 Hz / PWM clock / PWM range max
    // - PWM clock by hertz source - https://raspberrypi.stackexchange.com/a/38070
    wiringpi_pin_pwm         = get_wiringpi_pin_from_bcm_gpio( BCM_GPIO_PIN_PWM );
    float          pwm_clock = 19.2e6 / PWM_FREQ_HZ / MAX_DUTY_CYCLE;

    // Last fan state
    bool last_fan_enabled              = false;
    unsigned long last_above_min_epoch = time( NULL );

    #ifdef DEBUG

        // Tachometer tracking is only enabled during debug + when an appropriate pin configured
        if( BCM_GPIO_PIN_TACH > 0 ) {

            wiringpi_pin_tach = get_wiringpi_pin_from_bcm_gpio( BCM_GPIO_PIN_TACH );

            // Need to get time value as high precision to do spurious pull-down logic
            gettimeofday( &tach_last_fall_epoch, NULL );
        }

        printf( "\nRuntime:\n" );
        printf( " - wiringpi_pin_pwm              = %i\n",  wiringpi_pin_pwm );
        printf( " - pwm_range                     = %i\n",  MAX_DUTY_CYCLE );
        printf( " - pwm_clock                     = %f\n",  pwm_clock );
        printf( " - last_above_min_epoch          = %li\n", last_above_min_epoch );
        printf( " - wiringpi_pin_tach             = %i\n",  wiringpi_pin_tach );
        printf( " - tach_rpm                      = %i\n", tach_rpm );
        printf( " - tach_last_fall_epoch.tv_usec  = %li\n", tach_last_fall_epoch.tv_usec );

        printf( "\n" );

    #endif

    printf( "Starting PWM fan control service on GPIO pin #%i (WiringPi pin #%i)... <3 folkhack 2022\n", BCM_GPIO_PIN_PWM, wiringpi_pin_pwm );

    // Setup wiringPi
    if( wiringPiSetup() == -1 ) {

        printf( "Error initializing wiring PI library!\n" );
        return 1;
    }

    // Set the PWM pin to output mode (we do not need to read fan speeds)
    pinMode( wiringpi_pin_pwm, PWM_OUTPUT );
    pwmSetMode( PWM_MODE_MS );
    pwmSetRange( MAX_DUTY_CYCLE );
    pwmSetClock( pwm_clock );

    printf( "Wiring PI/GPIO initialized! GPIO #%i set to output + mark space mode at %iHz (%fHz clock)!\n", BCM_GPIO_PIN_PWM, PWM_FREQ_HZ, pwm_clock );

    #ifdef DEBUG

        if( BCM_GPIO_PIN_TACH ) {

            printf( "Tachometer support enabled on GPIO #%i! Setting up pull-down event handler...\n", BCM_GPIO_PIN_TACH );

            wiringPiISR( wiringpi_pin_tach, INT_EDGE_FALLING, on_tach_pull_down );

            printf( "Tachometer support setup!\n" );
        }

    #endif

    // PWM ready for duty cycle set
    is_setup = true;

    printf( "Blipping to full duty cycle %i for 10s...\n", MAX_DUTY_CYCLE );

    // Blip fan to full duty cycle before start
    pwmWrite( wiringpi_pin_pwm, MAX_DUTY_CYCLE );
    sleep( 10 );
    pwmWrite( wiringpi_pin_pwm, 0 );

    printf( "10s fan blip finished! Starting CPU temp polling at %is sleep interval...\n", SLEEP_S );

    ////////////////////////////////////////////////////////////////////////////////
    //
    //  Main loop
    //

    while( 1 ) {

        float cur_temp_c = get_cpu_temp_c();

        // Set fan to full if error reading CPU (denoted by -1 return value)
        if( cur_temp_c < 0 ) {

            printf( "Error reading CPU temp! Setting fan to full and exiting with code 1...\n" );

            pwmWrite( wiringpi_pin_pwm, MAX_DUTY_CYCLE );
            return 1;
        }

        unsigned short duty_cycle_set_val = 0;
        float use_min_temp_c              = MIN_ON_TEMP_C;

        // If fan is on, then we use MIN_OFF_TEMP_C else we use MIN_ON_TEMP_C to smooth on/off cycles
        if( last_fan_enabled ) {

            use_min_temp_c = MIN_OFF_TEMP_C;
        }

        // If we're above min off temp then set last_above_min_epoch
        if( cur_temp_c > use_min_temp_c ) {

            last_above_min_epoch = time( NULL );
        }

        // If we're below min temp and within fan off grace period set to min duty cycle
        if( cur_temp_c <= use_min_temp_c && time( NULL ) - last_above_min_epoch < FAN_OFF_GRACE_S ) {

            duty_cycle_set_val = MIN_DUTY_CYCLE;

            #ifdef DEBUG
                printf( CYAN "%f" RESET " BELOW     use_min_temp_c - MIN_DUTY_CYCLE   ", cur_temp_c );
            #endif

        } else if( cur_temp_c <= use_min_temp_c ) {

            duty_cycle_set_val = 0;
            
            #ifdef DEBUG
                printf( GREEN "%f" RESET " BELOW_OFF use_min_temp_c - OFF              ", cur_temp_c );
            #endif
        
        } else if( cur_temp_c >= MAX_TEMP_C ) {

            duty_cycle_set_val = MAX_DUTY_CYCLE;
            
            #ifdef DEBUG
                printf( RED "%f" RESET " ABOVE     MAX_TEMP_C - MAX_DUTY_CYCLE       ", cur_temp_c );
            #endif

        } else {

            duty_cycle_set_val = quartic_bezier_easing( cur_temp_c, MIN_OFF_TEMP_C, MAX_TEMP_C, MIN_DUTY_CYCLE, MAX_DUTY_CYCLE );
            
            #ifdef DEBUG
                printf( YELLOW "%f" RESET " ABOVE     MAX_TEMP_C - quartic_bezier_easing", cur_temp_c );
            #endif
        }

        pwmWrite( wiringpi_pin_pwm, duty_cycle_set_val );

        #ifdef DEBUG

            printf( " - Duty cycle = " MAGENTA "%i" RESET, duty_cycle_set_val );

            if( BCM_GPIO_PIN_TACH ) {

                printf( " - Tach RPM = " CYAN "%i" RESET, tach_rpm );
                tach_rpm = 0;
            }

            printf( "\n" );

        #endif

        // Sleep for the interval before running main loop again
        sleep( SLEEP_S );
    }

    return 0;
}