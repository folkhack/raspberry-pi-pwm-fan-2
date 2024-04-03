#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

////////////////////////////////////////////////////////////////////////////////
//
//  Constants
//

// Define logging levels
#define DEBUG 0
#define INFO  5
#define ERROR 10

// Colored output (only used for debugging output)
#define RED     "\x1b[31m"
#define GREEN   "\x1b[32m"
#define YELLOW  "\x1b[33m"
#define MAGENTA "\x1b[35m"
#define CYAN    "\x1b[36m"
#define RESET   "\x1b[0m"

// Define fan modes
#define FAN_BELOW_OFF 0
#define FAN_BELOW_MIN 1
#define FAN_ABOVE_EAS 2
#define FAN_ABOVE_MAX 3

// CPU temp out-of-bounds range where error is thrown (temp in C * 1000)
#define CPU_TEMP_OOB_LOW 0
#define CPU_TEMP_OOB_HIGH 120000

// When is the duty cycle considered out of range
// - High-end range is 50kHz (double that of our default Noctua fan)
// - Should account for most PWM fans on the consumer/industrial markets
#define DUTY_CYCLE_NS_OOB_LOW 0
#define DUTY_CYCLE_NS_OOB_HIGH 800000

// Use a timeout for polling so that we can detect 0 RPM
#define RPM_TIMEOUT_MS 100

// Define a minimum time between tach pulses to avoid spurious pulses
#define TACH_MIN_TIME_DELTA_MS 2

// Smooth temp bezier input array size
#define CPU_TEMP_SMOOTH_ARR_SIZE 4

////////////////////////////////////////////////////////////////////////////////
//
//  Global scope vars
//

// ENV CONFIG - Declare configuration variables w/expected type
unsigned short BCM_GPIO_PIN_PWM = 18,
               PWM_FREQ_HZ      = 2500,
               MIN_DUTY_CYCLE   = 20,
               MAX_DUTY_CYCLE   = 100,
               FAN_OFF_GRACE_MS = 60000;

// ENV CONFIG - Time to sleep in main loop
unsigned int SLEEP_MS = 250;

// ENV CONFIG - Temp ranges
float MIN_OFF_TEMP_C = 38,
      MIN_ON_TEMP_C  = 40,
      MAX_TEMP_C     = 46;

// Debug logging mode enabled
bool debug_logging_enabled = false;

// CSV debugging - disables all logs minus telemetry
bool csv_debug_logging_enabled = false;

// Is setup flag to know if writing to the PWM is safe
bool is_setup = false;

// Is tachometer enabled?
bool is_tach_enabled = false;

// Calculate the PWM duty cycle period in nano-seconds
unsigned int pwm_duty_cycle_period_ns;

// Keep chip number and channel number in broad scope for clean-up
unsigned char pwm_chip_num;
unsigned char pwm_channel_num;

// File descriptors for control through /sys/class
FILE *fd_pwm_chip_export                   = NULL;
FILE *fd_pwm_chip_unexport                 = NULL;
FILE *fd_pwm_channel_enable                = NULL;
FILE *fd_pwm_channel_set_duty_cycle        = NULL;
FILE *fd_pwm_channel_set_duty_cycle_period = NULL;

// File handler for the CPU temp
FILE *fd_cpu_temp = NULL;

// Last time above the minimum off temp
struct timeval last_above_min_epoch;

// Array of last X CPU temps to average for smoothing out bezier input
float cpu_temp_smooth_arr[ CPU_TEMP_SMOOTH_ARR_SIZE ] = {0};

// Setup a flag so we can notify the main loop to close when SIGINT is
//    caught and our halt is called
volatile sig_atomic_t halt_received = 0;

// Declare configuration variables w/expected type
unsigned short bcm_gpio_pin_tach,
               tach_pulse_per_rev;

// Track tachometer (only enabled during debug)
// - RPM as volatile since it will be referenced from multiple threads
volatile unsigned short tach_rpm = 0;
struct timeval tach_last_fall_epoch;

// True GPIO tachometer GPIO # from /sys/kernel/debug/gpio
char gpio_true_tach_num[4];

// GPIO file descriptors
FILE *fd_gpio_tach_export     = NULL;
FILE *fd_gpio_tach_unexport   = NULL;
FILE *fd_gpio_tach_active_low = NULL;
FILE *fd_gpio_tach_direction  = NULL;
FILE *fd_gpio_tach_edge       = NULL;

// GPIO file descriptors - polling
int fd_gpio_tach_value;

// Setup stuct for the GPIO polling file descriptor
struct pollfd poll_tach_gpio;

// Setup a mutex for the RPM monitoring thread
pthread_mutex_t mutex_tach_rpm = PTHREAD_MUTEX_INITIALIZER;

// Setup a thread for polling the GPIO
pthread_t polling_thread_tach;

////////////////////////////////////////////////////////////////////////////////
//
//  Functions
//

// SIGINT/SIGTERM handler
void handle_halt( int noop ) { halt_received = 1; }

// Logging function DEBUG|INFO|ERROR constant for level; proxies vprintf and
//    supports stderr for errors
void l( int level, char* message_str, ... ) {

    // CSV debugging ignores all debug/info logging
    if( level != ERROR && csv_debug_logging_enabled ) {

        return;
    }

    va_list args;

    va_start( args, message_str );

    switch( level ) {

        case DEBUG:

            if( debug_logging_enabled ) {

                vprintf( message_str, args );
            }

            break;

        case INFO:
            vprintf( message_str, args );
            break;

        case ERROR:
            vfprintf( stderr, message_str, args );
            break;
    }

    va_end( args );
}

// Get the PWM chip number from the BCM GPIO number
// - TODO: Test configurations and implement!
unsigned char get_pwm_chip_num_from_bcm_gpio( unsigned short gpio_pin ) {

    static const unsigned char lookup[] = {
        [18] = '2'
    };

    return lookup[ gpio_pin ];
}

// Get the PWM channel number from the BCM GPIO number
// - TODO: Test configurations and implement!
unsigned char get_pwm_channel_num_from_bcm_gpio( unsigned short gpio_pin ) {

    static const unsigned char lookup[] = {
        [18] = '2'
        // TOOD: test, add the rest
    };

    return lookup[ gpio_pin ];
}

// Enable/disable GPIO via sysfs
// - NOTE: Must come before clean_up function due to being used to clean-up GPIO
void gpio_set_export( bool is_enabled ) {

    l( INFO, "GPIO %s %s...\n", gpio_true_tach_num, is_enabled ? "exporting" : "un-exporting" );

    fprintf( is_enabled ? fd_gpio_tach_export : fd_gpio_tach_unexport, "%s", gpio_true_tach_num );
    fflush( is_enabled ? fd_gpio_tach_export : fd_gpio_tach_unexport );

    l( INFO, "GPIO %s %s!\n", gpio_true_tach_num, is_enabled ? "exported" : "un-exported" );
}

// Get the fan mode string from the integer representation
const char* get_fan_mode_str( int fan_mode_int ) {

    static const char* lookup[] = {
        "BELOW_OFF",
        "BELOW_MIN",
        "ABOVE_EAS",
        "ABOVE_MAX"
    };

    return lookup[ fan_mode_int ];
}

// Clean-up file descriptors and free the tachometer GPIO if needed
void clean_up() {

    l( DEBUG, "Freeing file descriptors...\n" );

    // Free PWM control resources:
    if( fd_pwm_chip_export != NULL ) {

        l( DEBUG, "Freeing fd_pwm_chip_export...\n" );
        fclose( fd_pwm_chip_export );
        fd_pwm_chip_export = NULL;
    }

    if( fd_pwm_chip_unexport != NULL ) {

        l( DEBUG, "Freeing fd_pwm_chip_unexport...\n" );
        fclose( fd_pwm_chip_unexport );
        fd_pwm_chip_unexport = NULL;
    }

    if( fd_pwm_channel_enable != NULL ) {

        l( DEBUG, "Freeing fd_pwm_channel_enable...\n" );
        fclose( fd_pwm_channel_enable );
        fd_pwm_channel_enable = NULL;
    }

    if( fd_pwm_channel_set_duty_cycle != NULL ) {

        l( DEBUG, "Freeing fd_pwm_channel_set_duty_cycle...\n" );
        fclose( fd_pwm_channel_set_duty_cycle );
        fd_pwm_channel_set_duty_cycle = NULL;
    }

    if( fd_pwm_channel_set_duty_cycle_period != NULL ) {

        l( DEBUG, "Freeing fd_pwm_channel_set_duty_cycle_period...\n" );
        fclose( fd_pwm_channel_set_duty_cycle_period );
        fd_pwm_channel_set_duty_cycle_period = NULL;
    }

    if( fd_cpu_temp != NULL ) {

        l( DEBUG, "Freeing fd_cpu_temp...\n" );
        fclose( fd_cpu_temp );
        fd_cpu_temp = NULL;
    }

    // Free tachometer resources:
    if( fd_gpio_tach_unexport != NULL ) {

        gpio_set_export( false );

        printf( "Freeing fd_gpio_tach_unexport...\n" );
        fclose( fd_gpio_tach_unexport );
        fd_gpio_tach_unexport = NULL;
    }

    if( fd_gpio_tach_export != NULL ) {

        printf( "Freeing fd_gpio_tach_export...\n" );
        fclose( fd_gpio_tach_export );
        fd_gpio_tach_export = NULL;
    }

    if( fd_gpio_tach_active_low != NULL ) {

        printf( "Freeing fd_gpio_tach_active_low...\n" );
        fclose( fd_gpio_tach_active_low );
        fd_gpio_tach_active_low = NULL;
    }

    if( fd_gpio_tach_direction != NULL ) {

        printf( "Freeing fd_gpio_tach_direction...\n" );
        fclose( fd_gpio_tach_direction );
        fd_gpio_tach_direction = NULL;
    }

    if( fd_gpio_tach_edge != NULL ) {

        printf( "Freeing fd_gpio_tach_edge...\n" );
        fclose( fd_gpio_tach_edge );
        fd_gpio_tach_edge = NULL;
    }

    if( fd_gpio_tach_value > 0 ) {

        printf( "Freeing fd_gpio_tach_value...\n" );
        close( fd_gpio_tach_value );
        fd_gpio_tach_value = -1;
    }

    l( DEBUG, "File descriptors freed!\n" );
}

// Clean-up and exit with code
void clean_up_and_exit( int exit_code ) {

    clean_up();

    if( exit_code != 0 ) {

        l( ERROR, "Exiting with POSIX status code %i... :(", exit_code );
        exit( exit_code );
    }

    l( INFO, "Exiting with POSIX status code 0... :D" );
    exit( 0 );
}

// Open a file descriptor at path with specific mode and die on failure
void open_fd( char* path_str, FILE **file_descriptor, char* mode ) {

    l( DEBUG, "Opening \"%s\" with mode %s...\n", path_str, mode );

    *file_descriptor = fopen( path_str, mode );

    if( *file_descriptor == NULL ) {

        l( ERROR, "Error opening \"%s\"... Exiting with status 1...\n", path_str );
        clean_up_and_exit( 1 );
    }

    l( DEBUG, "\"%s\" opened!...\n", path_str );
}

// Enable/disable the PWM chip control via sysfs
void pwm_set_chip_export_channel( bool is_enabled ) {

    l( DEBUG, "PWM channel %s...\n", is_enabled ? "exporting" : "un-exporting" );

    fprintf( is_enabled ? fd_pwm_chip_export : fd_pwm_chip_unexport, "%c", pwm_channel_num );
    fflush( is_enabled ? fd_pwm_chip_export : fd_pwm_chip_unexport );

    l( DEBUG, "PWM channel %s!\n", is_enabled ? "exported" : "un-exported" );
}

// Set the duty-cycle to scaled value
void pwm_set_duty_cycle( unsigned int duty_cycle ) {

    if( duty_cycle > MAX_DUTY_CYCLE ) {

        l( ERROR, "ERROR: Duty cycle exceeds maximum allowed value!\n" );
        return;
    }

    float duty_cycle_ns = ( ( float ) duty_cycle / MAX_DUTY_CYCLE ) * pwm_duty_cycle_period_ns;

    if( duty_cycle_ns < DUTY_CYCLE_NS_OOB_LOW || duty_cycle_ns > DUTY_CYCLE_NS_OOB_HIGH ) {

        l( ERROR, "ERROR: Duty cycle exceeds OOB range!\n" );
        return;
    }

    fprintf( fd_pwm_channel_set_duty_cycle, "%.0f", duty_cycle_ns );
    fflush( fd_pwm_channel_set_duty_cycle );
}

// Set the duty cycle to max, but ensure value chages so sysfs picks up change
void pwm_set_max_duty_cycle() {

    // Ensure if alreayd MAX_DUTY_CYCLE that atomic update is seen by sysfs
    pwm_set_duty_cycle( MAX_DUTY_CYCLE - 1 );
    pwm_set_duty_cycle( MAX_DUTY_CYCLE );
}

// Setup the PWM controller for fan control
void pwm_setup() {

    // Get PWM chip and channel numbers
    pwm_chip_num = get_pwm_chip_num_from_bcm_gpio( BCM_GPIO_PIN_PWM );
    pwm_channel_num = get_pwm_channel_num_from_bcm_gpio( BCM_GPIO_PIN_PWM );

    // Format to paths for /sys/class control
    char pwm_chip_path_str[25];
    char pwm_channel_path_str[30];

    snprintf( pwm_chip_path_str, sizeof( pwm_chip_path_str ), "/sys/class/pwm/pwmchip%c/", pwm_chip_num );
    snprintf( pwm_channel_path_str, sizeof( pwm_channel_path_str ), "%spwm%c/", pwm_chip_path_str, pwm_channel_num );

    char chip_unexport_str[33];
    snprintf( chip_unexport_str, sizeof( chip_unexport_str ), "%sunexport", pwm_chip_path_str );
    open_fd( chip_unexport_str, &fd_pwm_chip_unexport, "w" );

    // Ensure unloaded before we start
    pwm_set_chip_export_channel( false );

    // Setup file descriptors/handles for /sys/class control points
    char chip_export_str[31];
    snprintf( chip_export_str, sizeof( chip_export_str ), "%sexport", pwm_chip_path_str );
    open_fd( chip_export_str, &fd_pwm_chip_export, "w" );

    // Setup the chip export channel
    pwm_set_chip_export_channel( true );

    char channel_enable_path_str[37];
    snprintf( channel_enable_path_str, sizeof( channel_enable_path_str ), "%senable", pwm_channel_path_str );
    open_fd( channel_enable_path_str, &fd_pwm_channel_enable, "w" );

    // Allow channel to settle before enabling
    // sleep( 1 );

    char channel_set_duty_cycle_path_str[43];
    snprintf( channel_set_duty_cycle_path_str, sizeof( channel_set_duty_cycle_path_str ), "%sduty_cycle", pwm_channel_path_str );
    open_fd( channel_set_duty_cycle_path_str, &fd_pwm_channel_set_duty_cycle, "w" );

    char channel_set_duty_cycle_period_path_str[39];
    snprintf( channel_set_duty_cycle_period_path_str, sizeof( channel_set_duty_cycle_period_path_str ), "%speriod", pwm_channel_path_str );
    open_fd( channel_set_duty_cycle_period_path_str, &fd_pwm_channel_set_duty_cycle_period, "w" );

    // Setup PWM duty cycle period
    pwm_duty_cycle_period_ns = ( 1000000000 / PWM_FREQ_HZ );
    
    l( DEBUG, "Setting duty cycle period to %u...\n", pwm_duty_cycle_period_ns );

    fprintf( fd_pwm_channel_set_duty_cycle_period, "%u", pwm_duty_cycle_period_ns );
    fflush( fd_pwm_channel_set_duty_cycle_period );

    l( DEBUG, "Duty cycle period set to %u!\n", pwm_duty_cycle_period_ns );

    // Set the channel to enabled
    l( DEBUG, "PWM channel enabling...\n" );

    fprintf( fd_pwm_channel_enable, "1" );
    fflush( fd_pwm_channel_enable );

    l( DEBUG, "PWM channel enabled!\n" );

    // Set the last time we were above minimum off temp to now
    gettimeofday( &last_above_min_epoch, NULL );

    l( DEBUG, "\nRuntime:\n" );
    l( DEBUG, " - BCM_GPIO_PIN_PWM         = %i\n",  BCM_GPIO_PIN_PWM );
    l( DEBUG, " - pwm_chip_num             = %i\n",  pwm_chip_num );
    l( DEBUG, " - pwm_channel_num          = %i\n",  pwm_channel_num );
    l( DEBUG, " - pwm_chip_path_str        = %s\n",  pwm_chip_path_str );
    l( DEBUG, " - pwm_channel_path_str     = %s\n",  pwm_channel_path_str );
    l( DEBUG, " - pwm_duty_cycle_period_ns = %i\n",  pwm_duty_cycle_period_ns );
    l( DEBUG, " - MAX_DUTY_CYCLE           = %i\n",  MAX_DUTY_CYCLE );
    l( DEBUG, " - last_above_min_epoch     = %li\n", last_above_min_epoch.tv_sec );
    l( DEBUG, "\n" );

    // CPU temp setup
    // `/sys/class/thermal/thermal_zone0/temp` on Raspberry Pi contains current temp
    //    in Celsius * 1000
    open_fd( "/sys/class/thermal/thermal_zone0/temp", &fd_cpu_temp, "r" );

    is_setup = true;
}

// Get CPU temp as float
float get_cpu_temp_c() {

    // Value in "temp" file is degrees in C * 1000
    float cpu_temp_raw;

    // Read the temp into the `cpu_temp_raw` variable by reference
    fflush( fd_cpu_temp );
    rewind( fd_cpu_temp );
    fscanf( fd_cpu_temp, "%f", &cpu_temp_raw );

    // Check if within reasonable range temps and return -1 to denote issue
    if( cpu_temp_raw <= CPU_TEMP_OOB_LOW || cpu_temp_raw >= CPU_TEMP_OOB_HIGH ) {

        return -1;
    }

    // Convert to correct Celsius temp double
    float cpu_temp_c = cpu_temp_raw / 1000;

    // Shift the existing elements to the right
    for( int i = CPU_TEMP_SMOOTH_ARR_SIZE - 1; i > 0; i-- ) {

        cpu_temp_smooth_arr[ i ] = cpu_temp_smooth_arr[ i - 1 ];
    }

    // Set the value
    cpu_temp_smooth_arr[0] = cpu_temp_c;

    return cpu_temp_c;
}

// Get CPU temp average
float get_cpu_temp_avg_c() {

    float sum = 0.0;

    for( int i = 0; i < CPU_TEMP_SMOOTH_ARR_SIZE; i++ ) {

        sum += cpu_temp_smooth_arr[ i ];
    }

    return sum / CPU_TEMP_SMOOTH_ARR_SIZE;
}

// Quartic bezier easing function
// - https://easings.net/#easeInOutQuart
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

// Get the true GPIO number that's associated to the BCM GPIO # provided so
//    we can look the device up in the sysfs interface
void get_true_gpio_num( int gpio_number, char* output ) {

    const char* path_str = "/sys/kernel/debug/gpio";

    // Get a file descriptor for the GPIO debug/status file
    FILE* fd = fopen( path_str, "r" );

    if( fd == NULL ) {

        l( ERROR, "Error opening \"%s\"\n", path_str );
        clean_up_and_exit( 1 );
    }

    // Buffer for lines
    char line[256];

    // Buffer for search string
    char search_string[6];

    // Format search string (looking for GPIO{num})
    sprintf( search_string, "GPIO%i", gpio_number );
    l( INFO, "Searching for \"%s\" in %s\n", search_string, path_str );

    // Iterate over file contents by line
    while( fgets( line, sizeof( line ), fd ) != NULL ) {

        // When line is found
        if( strstr( line, search_string ) ) {

            // Advance a pointer to the first found digit
            char* ptr = line;

            while( *ptr && ! ( *ptr >= '0' && *ptr <= '9' ) ) { ptr++; }

            // Found a digit, start extracting
            if( *ptr ) {

                int num_len = 0;

                while( *ptr >= '0' && *ptr <= '9' ) {

                    output[ num_len++ ] = *ptr++;
                }

                // Null-terminate the string
                output[ num_len ] = '\0';

                // Release the file descriptor and return
                fclose( fd );
                return;
            }

            // Found the correct line but no number, exit the loop
            break;
        }
    }

    // Release the file descriptor
    fclose( fd );

    l( ERROR, "Could not find proper GPIO number! Exiting with status 1...\n" );
    exit( 1 );
}

// Handler for tachometer pull-down (ie: rotation pulse)
void on_tach_pull_down() {

    struct timeval cur_epoch;
    gettimeofday( &cur_epoch, NULL );

    float delta_time_ms = ( cur_epoch.tv_sec - tach_last_fall_epoch.tv_sec ) * 1000.0f + ( cur_epoch.tv_usec - tach_last_fall_epoch. tv_usec) / 1000.0f;

    gettimeofday( &tach_last_fall_epoch, NULL );

    // Reject spuriously short pulses
    if( delta_time_ms < TACH_MIN_TIME_DELTA_MS ) return;

    pthread_mutex_lock( &mutex_tach_rpm );

    tach_rpm = round( ( 1000.0 / delta_time_ms ) / tach_pulse_per_rev * 60 );

    pthread_mutex_unlock( &mutex_tach_rpm );
}

// Setup the GPIO polling interrupt for the tachomter using the true GPIO number
void setup_tach_gpio_interrupt( char* true_gpio_num ) {

    l( INFO, "Setting up GPIO polling interrupt on true GPIO #%s...\n", true_gpio_num );

    char gpio_value_path[32];
    snprintf( gpio_value_path, sizeof( gpio_value_path ), "/sys/class/gpio/gpio%s/value", true_gpio_num );

    fd_gpio_tach_value = open( gpio_value_path, O_RDONLY | O_NONBLOCK );

    if( fd_gpio_tach_value < 0 ) {

        l( ERROR, "Failed to open GPIO value file %s, error: %s\n", gpio_value_path, strerror( errno ) );
        clean_up_and_exit( 1 );
    }

    // Dummy read to clear any initial value
    char dumb_buffer[2];
    read( fd_gpio_tach_value, dumb_buffer, sizeof( dumb_buffer ) );

    // Setup polling file descriptor
    poll_tach_gpio.fd = fd_gpio_tach_value;

    // Priority data (rising or rising edge)
    poll_tach_gpio.events = POLLPRI;

    l( INFO, "GPIO polling interrupt setup on true GPIO #%s!\n", true_gpio_num );
}

// Setup the tachometer for measuring fan RPM
void tach_gpio_setup() {

    l( INFO, "Tachometer support enabled on GPIO #%i! Setting up pull-down event handler...\n", bcm_gpio_pin_tach );

    get_true_gpio_num( bcm_gpio_pin_tach, gpio_true_tach_num );
    l( INFO, "Tachometer true GPIO found: %s\n", gpio_true_tach_num );

    char gpio_path_str[17] = "/sys/class/gpio/";

    // Ensure unloaded before we start
    open_fd( "/sys/class/gpio/unexport", &fd_gpio_tach_unexport, "w" );
    gpio_set_export( false );

    // Setup file descriptors/handles for /sys/class control points
    open_fd( "/sys/class/gpio/export", &fd_gpio_tach_export, "w" );
    gpio_set_export( true );

    char gpio_pin_path_str[32];
    snprintf( gpio_pin_path_str, sizeof( gpio_pin_path_str ), "%sgpio%s/", gpio_path_str, gpio_true_tach_num );

    char gpio_active_low_path_str[48];
    snprintf( gpio_active_low_path_str, sizeof( gpio_active_low_path_str ), "%sactive_low", gpio_pin_path_str );
    open_fd( gpio_active_low_path_str, &fd_gpio_tach_active_low, "w" );

    char gpio_direction_path_str[48];
    snprintf( gpio_direction_path_str, sizeof( gpio_direction_path_str ), "%sdirection", gpio_pin_path_str );
    open_fd( gpio_direction_path_str, &fd_gpio_tach_direction, "w" );

    char gpio_edge_path_str[64];
    snprintf( gpio_edge_path_str, sizeof( gpio_edge_path_str ), "%sedge", gpio_pin_path_str );
    open_fd( gpio_edge_path_str, &fd_gpio_tach_edge, "w" );

    l( INFO, "Setting active low to 0...\n" );

    fprintf( fd_gpio_tach_active_low, "0" );
    fflush( fd_gpio_tach_active_low );

    l( INFO, "Active low set to 0! Setting direction to \"in\"...\n" );

    fprintf( fd_gpio_tach_direction, "in" );
    fflush( fd_gpio_tach_direction );

    l( INFO, "Direction set to \"in\"! Setting edge to \"falling\"...\n" );

    fprintf( fd_gpio_tach_edge, "falling" );
    fflush( fd_gpio_tach_edge );

    l( INFO, "Edge set to \"falling\"!\n" );

    setup_tach_gpio_interrupt( gpio_true_tach_num );

    l( INFO, "Tachometer support setup!\n" );
}

// Polling thread function for the tachometer
// - Uses own thread for independent polling loop to monitor for GPIO events
void* polling_thread_tach_func(void* arg) {

    char dumb_buffer[64];
    int poll_return;

    // Track time since last pulse so we can detect 0 RPM
    struct timeval last_pulse_time, current_time;
    float time_since_last_pulse_ms;

    // Get the current time as the initial last pulse time
    gettimeofday( &last_pulse_time, NULL );

    while( ! halt_received ) {

        // Wait for an event on the GPIO pin
        poll_return = poll( &poll_tach_gpio, 1, RPM_TIMEOUT_MS );

        if( poll_return > 0 && poll_tach_gpio.revents & POLLPRI ) {

            // Reset the file pointer to read from the start
            lseek( poll_tach_gpio.fd, 0, SEEK_SET );

            // Read to clear the event
            read( poll_tach_gpio.fd, dumb_buffer, sizeof( dumb_buffer ) );

            // Calculate the RPM
            on_tach_pull_down();

            // Update the last pulse time
            gettimeofday( &last_pulse_time, NULL );

        } else {

            // Either timeout or error, check the time since the last pulse
            gettimeofday(&current_time, NULL);
            time_since_last_pulse_ms = ( current_time.tv_sec - last_pulse_time.tv_sec ) * 1000.0f + ( current_time.tv_usec - last_pulse_time.tv_usec ) / 1000.0f;

            // If the time since the last pulse exceeds our threshold, set RPM to 0
            if( time_since_last_pulse_ms >= RPM_TIMEOUT_MS ) {

                pthread_mutex_lock( &mutex_tach_rpm );
                tach_rpm = 0;
                pthread_mutex_unlock( &mutex_tach_rpm );
            }
        }
    }

    pthread_exit( NULL );
}

// Setup the tachometer polling thread
void tach_polling_setup() {

    int thread_create_status;

    // Creating the polling thread
    thread_create_status = pthread_create( &polling_thread_tach, NULL, polling_thread_tach_func, NULL );

    if( thread_create_status != 0 ) {

        l( ERROR, "Failed to create the polling thread\n" );
        clean_up_and_exit( 1 );
    }
}

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

        l( INFO, "\nRaspberry Pi CPU PWM Fan Controller v2 \n"
                 "\n"
                 "Usage: ./pwm_fan_control2 {tach_pin optional} {tach_pulse_per_rotation optional}\n"
                 "\n"
                 " - Watches CPU temp and sets PWM fan speed accordingly.\n"
                 " - Configured through environment variables.\n"
                 " - See readme.md for documentation.\n"
                 "\n"
                 "Examples:\n"
                 "\n"
                 "  Show this help:\n"
                 "    ./pwm_fan_tach2 --help\n"
                 "\n"
                 "  Run:\n"
                 "    ./pwm_fan_tach2\n"
                 "\n"
                 "  Run w/debug logging:\n"
                 "    ./pwm_fan_tach2 debug\n"
                 "\n"
                 "  Run w/CSV debug logging:\n"
                 "    ./pwm_fan_tach2 csvdebug\n"
                 "\n"
                 "  Run w/debug logging + tachometer on GPIO pin #24 with 2 pulses per revolution:\n"
                 "    ./pwm_fan_tach2 debug 24 2\n"
                 "\n"
                 "Exit status:\n"
                 "  0 if OK\n"
                 "  1 if error\n"
                 "\n"
                 "Online help, docs & bug reports: <https://github.com/folkhack/raspberry-pi-pwm-fan-2> \n"
        );

        return 0;
    }

    // Enable debugging
    if( argc > 1 && strcmp( argv[1], "debug" ) == 0 ) {

        debug_logging_enabled = true;
    }

    // Enable CSV debugging
    if( argc > 1 && strcmp( argv[1], "csvdebug" ) == 0 ) {

        csv_debug_logging_enabled = true;
    }

    // Check if the required number of arguments is provided if using tachometer
    if( argc > 2 && argc != 4 ) {

        l( ERROR, "Error: Incorrect number of arguments.\n" );
        l( ERROR, "Use --help for usage information.\n" );

        clean_up_and_exit( 1 );
    }

    is_tach_enabled = argc == 4;

    ////////////////////////////////////////////////////////////////////////////////
    //
    //  Config - set from environment variables
    //  - See readme.md for documentation
    //

    if( getenv( "PWM_FAN_BCM_GPIO_PIN_PWM" ) ) sscanf( getenv( "PWM_FAN_BCM_GPIO_PIN_PWM" ), "%hu", &BCM_GPIO_PIN_PWM );;
    if( getenv( "PWM_FAN_PWM_FREQ_HZ" ) )      sscanf( getenv( "PWM_FAN_PWM_FREQ_HZ" ),      "%hu", &PWM_FREQ_HZ );
    if( getenv( "PWM_FAN_MIN_DUTY_CYCLE" ) )   sscanf( getenv( "PWM_FAN_MIN_DUTY_CYCLE" ),   "%hu", &MIN_DUTY_CYCLE );
    if( getenv( "PWM_FAN_MAX_DUTY_CYCLE" ) )   sscanf( getenv( "PWM_FAN_MAX_DUTY_CYCLE" ),   "%hu", &MAX_DUTY_CYCLE );
    if( getenv( "PWM_FAN_FAN_OFF_GRACE_MS" ) ) sscanf( getenv( "PWM_FAN_FAN_OFF_GRACE_MS" ), "%hu", &FAN_OFF_GRACE_MS );
    if( getenv( "PWM_FAN_SLEEP_MS" ) )         sscanf( getenv( "PWM_FAN_SLEEP_MS" ),         "%i",  &SLEEP_MS );
    if( getenv( "PWM_FAN_MIN_OFF_TEMP_C" ) )   sscanf( getenv( "PWM_FAN_MIN_OFF_TEMP_C" ),   "%f",  &MIN_OFF_TEMP_C );
    if( getenv( "PWM_FAN_MIN_ON_TEMP_C" ) )    sscanf( getenv( "PWM_FAN_MIN_ON_TEMP_C" ),    "%f",  &MIN_ON_TEMP_C );
    if( getenv( "PWM_FAN_MAX_TEMP_C" ) )       sscanf( getenv( "PWM_FAN_MAX_TEMP_C" ),       "%f",  &MAX_TEMP_C );

    l( DEBUG, "\nConfig:\n" );
    l( DEBUG, " - BCM_GPIO_PIN_PWM = %i\n", BCM_GPIO_PIN_PWM );
    l( DEBUG, " - PWM_FREQ_HZ      = %i\n", PWM_FREQ_HZ );
    l( DEBUG, " - MIN_DUTY_CYCLE   = %i\n", MIN_DUTY_CYCLE );
    l( DEBUG, " - MAX_DUTY_CYCLE   = %i\n", MAX_DUTY_CYCLE );
    l( DEBUG, " - MIN_OFF_TEMP_C   = %f\n", MIN_OFF_TEMP_C );
    l( DEBUG, " - MIN_ON_TEMP_C    = %f\n", MIN_ON_TEMP_C );
    l( DEBUG, " - MAX_TEMP_C       = %f\n", MAX_TEMP_C );
    l( DEBUG, " - FAN_OFF_GRACE_MS = %i\n", FAN_OFF_GRACE_MS );
    l( DEBUG, " - SLEEP_MS         = %i\n", SLEEP_MS );
    l( DEBUG, "\n" );

    for( int i = CPU_TEMP_SMOOTH_ARR_SIZE - 1; i > 0; i-- ) { cpu_temp_smooth_arr[i] = MAX_TEMP_C; }

    ////////////////////////////////////////////////////////////////////////////////
    //
    //  Runtime setup
    //

    l( INFO, "Starting PWM fan controller...\n" );

    // Setup CSV headers if needed
    if( csv_debug_logging_enabled ) {

        printf( "cur_temp_c,decided_mode,duty_cycle_set_val" );

        if( is_tach_enabled ) {

            printf( ",tach_rpm" );
        }

        printf( "\n" );
    }

    pwm_setup();

    if( is_tach_enabled ) {

        l( INFO, "Starting tachometer...\n" );

        bcm_gpio_pin_tach  = ( unsigned short ) strtoul( argv[2], NULL, 10 );
        tach_pulse_per_rev = ( unsigned short ) strtoul( argv[3], NULL, 10 );

        l( INFO, "Monitoring GPIO pin: %d, Pulses per revolution: %d\n", bcm_gpio_pin_tach, tach_pulse_per_rev );

        tach_gpio_setup();
        tach_polling_setup();
    }

    l( INFO, "Blipping to full duty cycle %i for 2s...\n", MAX_DUTY_CYCLE );

    // Blip fan to full duty cycle before start
    pwm_set_max_duty_cycle();
    sleep( 2 );

    l( INFO, "2s fan blip finished! Starting main loop CPU temp polling/PWM set at %ins sleep interval...\n", SLEEP_MS );

    ////////////////////////////////////////////////////////////////////////////////
    //
    //  Main loop
    //
    struct timeval cur_epoch;
    unsigned short duty_cycle_set_val;
    float cur_temp_c;
    float use_min_temp_c;
    float grace_check_ms;
    unsigned short decided_mode_int;

    while( ! halt_received ) {

        cur_temp_c = get_cpu_temp_c();

        // Set fan to full if error reading CPU
        if( cur_temp_c <= 0 ) {

            l( ERROR, "ERROR: Invalid CPU temp! Setting fan to full for safety and continuing...\n" );
            pwm_set_max_duty_cycle();

            // Sleep and continue
            usleep( SLEEP_MS );
            continue;
        }

        // Push temp to 

        duty_cycle_set_val = 0;
        use_min_temp_c     = MIN_ON_TEMP_C;

        // If we're above min off temp then set last_above_min_epoch
        if( cur_temp_c > use_min_temp_c ) {

            gettimeofday( &last_above_min_epoch, NULL );
        }

        gettimeofday( &cur_epoch, NULL );

        grace_check_ms = ( cur_epoch.tv_sec - last_above_min_epoch.tv_sec ) * 1000.0f + ( cur_epoch.tv_usec - last_above_min_epoch.tv_usec ) / 1000.0f;

        // If we're below min temp and within fan off grace period set to min duty cycle
        if( cur_temp_c <= use_min_temp_c && grace_check_ms < FAN_OFF_GRACE_MS ) {

            duty_cycle_set_val = MIN_DUTY_CYCLE;
            decided_mode_int   = FAN_BELOW_MIN;

            l( DEBUG, CYAN "%.2f" RESET " BELOW_MIN use_min_temp_c - MIN_DUTY_CYCLE   ", cur_temp_c );

        } else if( cur_temp_c <= use_min_temp_c ) {

            duty_cycle_set_val = 0;
            decided_mode_int   = FAN_BELOW_OFF;

            l( DEBUG, GREEN "%.2f" RESET " BELOW_OFF use_min_temp_c - OFF              ", cur_temp_c );

        } else if( cur_temp_c >= MAX_TEMP_C ) {

            duty_cycle_set_val = MAX_DUTY_CYCLE;
            decided_mode_int   = FAN_ABOVE_MAX;

            l( DEBUG, RED "%.2f" RESET " ABOVE_MAX MAX_TEMP_C - MAX_DUTY_CYCLE       ", cur_temp_c );

        } else {

            duty_cycle_set_val = quartic_bezier_easing( get_cpu_temp_avg_c(), MIN_OFF_TEMP_C, MAX_TEMP_C, MIN_DUTY_CYCLE, MAX_DUTY_CYCLE );
            decided_mode_int   = FAN_ABOVE_EAS;

            l( DEBUG, YELLOW "%.2f" RESET " ABOVE_EAS MAX_TEMP_C - quartic_bezier_easing", cur_temp_c );
        }

        pwm_set_duty_cycle( duty_cycle_set_val );
        l( DEBUG, " - DC = " MAGENTA "%i" RESET, duty_cycle_set_val );

        // Handle CSV logging
        if( csv_debug_logging_enabled ) {

            printf( "%.2f,%s,%i", cur_temp_c, get_fan_mode_str( decided_mode_int ), duty_cycle_set_val );

            if( is_tach_enabled ) {

                printf( ",%u", tach_rpm );
            }

            printf( "\n" );
        }

        // Output tachometer if needed
        if( is_tach_enabled ) {

            l( DEBUG, " - RPM = " CYAN "%i" RESET, tach_rpm );
            tach_rpm = 0;
        }

        l( DEBUG, "\n" );

        usleep( SLEEP_MS * 1000 );
    }

    l( INFO, "Halt recieved!\n" );

    if( is_setup ) {

        l( INFO, "Setting to MAX_DUTY_CYCLE %i before exit...\n", MAX_DUTY_CYCLE );
        pwm_set_max_duty_cycle();
    }

    if( is_tach_enabled ) {

        l( INFO, "Waiting for tachometer polling thread to finish...\n" );

        pthread_join( polling_thread_tach, NULL );

        l( INFO, "Tachometer polling thread to finished!\n" );
    }

    clean_up_and_exit( 0 );
    return 0;
}