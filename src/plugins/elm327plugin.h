#ifndef ELM327PLUGIN_H
#define ELM327PLUGIN_H

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ELM327_RESET    		"ATWS"
#define ELM327_VERSION			"ATI"
#define ELM327_ECHO_OFF 		"AT E0"
#define ELM327_LINEFEEDS_OFF	"AT L0"
#define ELM327_EOC      		"\r"

#define ELM327_OK       		"OK"				// command successfully executed
#define ELM327_INVALID			"?"					// ELM could not understand the command
#define ELM327_ACT_ALERT		"ACT ALERT"			// no activity for the last 19 minutes, going into low power mode soon
#define ELM327_BUFFER_FULL		"BUFFER FULL"		// ELM TX Buffer full
#define ELM327_BUS_BUSY			"BUS BUSY"			// CAN bus was to busy to request PID
#define ELM327_BUS_ERROR		"BUS ERROR"			// Generic bus error.
#define ELM327_CAN_ERROR		"CAN ERROR"			// Generic CAN error. (Incorrect CAN baudrate, etc)
#define ELM327_DATA_ERROR		"DATA ERROR"		// Vehicle replied with invalid data (maybe checksum error)
#define ELM327_LINE_DATA_ERROR	"<DATA ERROR"		// A Data error ocurred in this exact line
#define ELM327_FEEDBACK_ERROR	"FB ERROR"			
#define ELM327_LOW_POWER_ALERT	"LP ALERT"			// ELM is going into low power mode in 2 seconds
#define ELM327_LOW_VOLTAGE_RESET	"LV RESET"			// ELM was undervolted & shut down.
#define ELM327_NO_DATA   		"NO DATA"			// protocol known, but OBDII port didn't reply to PID request
#define ELM327_LINE_RX_ERROR	"<RX ERROR"			// RX error in this line
#define ELM327_STOPPED			"STOPPED"			// ELM was interrupted by GPIO
#define ELM327_NOCONN   		"UNABLE TO CONNECT"	// ELM couldn't communicate with OBD port with any protocol
#define ELM327_SEARCHING		"SEARCHING..."		// ELM is trying to find out which protocol to use

#define OBDII_PID_SUPP1                 	0x00
#define OBDII_PID_ENGINE_LOAD               0x04
#define OBDII_PID_ENGINE_COOLANT_TEMP		0x05
#define OBDII_PID_FUEL_PRESSURE				0x0A
#define OBDII_PID_INTAKE_MANIFOLD_PRESSURE	0x0B
#define OBDII_PID_ENGINE_RPM				0x0C
#define OBDII_PID_VEHICLE_SPEED				0x0D
#define OBDII_PID_TIMING_ADVANCE			0x0E
#define OBDII_PID_INTAKE_AIR_TEMP			0x0F
#define OBDII_PID_MAF_AIR_FLOW				0x10
#define OBDII_PID_THROTTLE_POSITION			0x11
#define OBDII_PID_OBD_STANDARD				0x1C
#define OBDII_PID_AUX_INPUT_STATUS			0x1E
#define OBDII_PID_RUN_TIME					0x1F
#define OBDII_PID_SUPP2						0x20
#define OBDII_PID_FUEL_RAIL_PRESSURE		0x22
#define OBDII_PID_FUEL_RAIL_GAUGE_PRESSURE	0x23
#define OBDII_PID_EGR						0x2C
#define OBDII_PID_FUEL_LEVEL				0x2F
#define OBDII_PID_VAPOR_PRESSURE			0x32
#define OBDII_PID_BAROMETRIC_PRESSURE		0x33
#define OBDII_PID_SUPP3						0x40
#define OBDII_PID_CONTROL_MODULE_VOLTAGE	0x42
#define OBDII_PID_ABSOLUTE_LOAD				0x43
#define OBDII_PID_RELATIVE_THROTTLE_POSITION	0x45
#define OBDII_PID_AMBIENT_AIR_TEMPERATURE	0x46
#define OBDII_PID_ETHANOL_FUEL_PERCENT		0x52
#define OBDII_PID_ENGINE_OIL_TEMPERATURE	0x5C
#define OBDII_PID_FUEL_INJECTION_TIMING		0x5D
#define OBDII_PID_ENGINE_FUEL_RATE			0x5E
#define OBDII_PID_SUPP4						0x60
#define OBDII_PID_DEMANDED_PERCENT_TORQUE	0x61
#define OBDII_PID_ACTUAL_PERCENT_TORQUE		0x62
#define OBDII_PID_REFERENCE_TORQUE			0x63

enum elm327plugin_errno {
	ELM_ERRNO_OK,
	ELM_ERRNO_INVALID,
	ELM_ERRNO_ACT_ALERT,
	ELM_ERRNO_BUFFER_FULL,
	ELM_ERRNO_BUS_BUSY,
	ELM_ERRNO_BUS_ERROR,
	ELM_ERRNO_CAN_ERROR,
	ELM_ERRNO_DATA_ERROR,
	ELM_ERRNO_LINE_DATA_ERROR,
	ELM_ERRNO_FEEDBACK_ERROR,
	ELM_ERRNO_LOW_POWER_ALERT,
	ELM_ERRNO_LOW_VOLTAGE_RESET,
	ELM_ERRNO_NO_DATA,
	ELM_ERRNO_LINE_RX_ERROR,
	ELM_ERRNO_STOPPED,
	ELM_ERRNO_NOCONN,
	ELM_ERRNO_SEARCHING
};

struct elm327 {
	char version[64];
	uint32_t supported_pids[8];
	struct termios tty;
	struct timespec timeout;
	fd_set fdset;
	int fd;
	int baudrate;
	bool is_online;
	enum elm327plugin_errno elm_errno;
};

struct pidqq_element;
typedef void (*ELM327PluginPIDQueryCompletionCallback)(struct pidqq_element query, uint32_t result, enum elm327plugin_errno elm_errno);

// pid queries take time to execute, so we queue all queries.
// every query has priority (so it's actually a priority queue)
struct pidqq_element {
    unsigned int priority;  // priority == 0 means the element does not exist.
                            // queries with higher priority will be
                            // executed first.
    uint8_t pid;            // the pid to be queried.
    char   *channel;
    bool    repeat;         // whether the query should be reinserted into the queue
                            // after it was executed.
    ELM327PluginPIDQueryCompletionCallback completionCallback;
};


#define ELM327PLUGIN_CHANNEL "plugins.flutter-pi.io/elm327"
#define ELM327PLUGIN_RPM_CHANNEL "plugins.flutter-pi.io/elm327/rpm"
#define ELM327PLUGIN_ENGINELOAD_CHANNEL "plugins.flutter-pi.io/elm327/engineload"
#define ELM327PLUGIN_COOLANTTEMP_CHANNEL "plugins.flutter-pi.io/elm327/coolanttemp"
#define ELM327PLUGIN_SPEED_CHANNEL "plugins.flutter-pi.io/elm327/speed"
#define ELM327PLUGIN_THROTTLE_CHANNEL "plugins.flutter-pi.io/elm327/throttle"

#define ELM327PLUGIN_DEVICE_PATH "/dev/rfcomm0"
#define ELM327PLUGIN_BAUDRATE 9600

int ELM327Plugin_init(void);
int ELM327Plugin_deinit(void);

#endif