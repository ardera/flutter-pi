#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
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

#include <pluginregistry.h>
#include "elm327plugin.h"


struct elm327         elm;
pthread_mutex_t       pidqq_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t        pidqq_added = PTHREAD_COND_INITIALIZER;
struct pidqq_element *pidqq = NULL;
size_t                pidqq_size = 0;
size_t                pidqq_nelements = 0;
atomic_bool           pidqq_processor_shouldrun = true;
pthread_t             pidqq_processor_thread;

/*****************************
 * Communication with ELM327 *
 *****************************/

/// send a raw command to the ELM327.
/// the contents of the cmd string and an end-of-command marker are
/// send to the ELM.
/// if response is not NULL and length is larger than 0, the ELM327's response
///   is stored in "response". If the buffer given by response and length is to small
///   to fit the ELMs response, elm_command will start writing at the start of the
///   buffer again.
/// if no response buffer is given (the response parameter is NULL or length is 0)
///   elm_command will still wait for a response, it just won't store anything.
int  elm_command(char *cmd, char *response, size_t length) {
	int ok = 0, count = 0, i = 0, ntransmit;
	char charbuff = 0;
	elm.elm_errno = ELM_ERRNO_OK;

	if (!elm.is_online) {
		fprintf(stderr, "[elm327plugin] elm_command: ELM327 must be online\n");
		return EINVAL;
	}
	if (!cmd) {
		fprintf(stderr, "[elm327plugin] elm_command: command can't be NULL\n");
		return EINVAL;
	}

	FlutterEngineTraceEventDurationBegin("elm_command");
	FlutterEngineTraceEventDurationBegin("elm_command write");

	// write cmd to line
	ok = pselect(elm.fd+1, NULL, &elm.fdset, NULL, &elm.timeout, NULL);
	if (ok > 0) {
		tcflush(elm.fd, TCIOFLUSH); 

		// why do we write byte per byte here?
		count += write(elm.fd, (const void *) cmd, sizeof(char)*strlen(cmd));
		count += write(elm.fd, (const void *) ELM327_EOC, sizeof(char)*strlen(ELM327_EOC));
		/*for (i=0; i < strlen(cmd); i++)
			count += write(elm.fd, (const void *) cmd+i, sizeof(char));
		
		for (i=0; i < strlen(ELM327_EOC); i++)
			count += write(elm.fd, (const void *) ELM327_EOC+i, sizeof(char));
		*/

		if (count != (strlen(cmd) + strlen(ELM327_EOC))) {
			fprintf(stderr, "[elm327plugin] elm_command: could not write command to serial, written %d bytes, should be %ld\n", count, (strlen(cmd) + strlen(ELM327_EOC)));
			elm.elm_errno = ELM_ERRNO_NOCONN;
			return EIO;
		}
	} else {
		fprintf(stderr, "[elm327plugin] elm_command: elm connection timed out while writing, after %lus, %09luns\n", elm.timeout.tv_sec, elm.timeout.tv_nsec);
		elm.elm_errno = ELM_ERRNO_NOCONN;
		return EIO;
	}

	FlutterEngineTraceEventDurationEnd("elm_command write");
	FlutterEngineTraceEventDurationBegin("elm_command read");

	// read response
	i = 0;
	while (1) {
		ok = read(elm.fd, &charbuff, sizeof(char));

		if (ok == 0) {
			fprintf(stderr, "[elm327plugin] elm_command: ELM327 connection timed out while reading, after %lus, %09luns\n", elm.timeout.tv_sec, elm.timeout.tv_nsec);
			elm.elm_errno = ELM_ERRNO_NOCONN;
			return EIO;
		} if (charbuff == '>') {
			if (response) response[i] = '\0';
			break;
		} else if (response && isprint(charbuff)) {
			response[i] = charbuff;
			i = (i+1) % length;
		}
	}

	FlutterEngineTraceEventDurationEnd("elm_command read");
	FlutterEngineTraceEventDurationEnd("elm_command");

	return 0;
}

/// queries the value of a pid
///   (uses elm_command for execution)
int  elm_query(uint8_t pid, uint32_t* response) {
	char txt_response[32], command[6];
	size_t response_length = 0;
	uint8_t bytes[6] = {0};
	int ok;

	elm.elm_errno = ELM_ERRNO_OK;

	sprintf(command, "01%02X1", pid);
	printf("[elm327plugin] query string: %s\n", command);

	ok = elm_command(command, txt_response, 32);
	if (ok != 0) return ok;

	// scan reply for errors
	elm.elm_errno = ELM_ERRNO_OK;
	if (strstr(txt_response, "ERROR")) {
		if (strstr(txt_response, ELM327_BUS_ERROR))
			elm.elm_errno = ELM_ERRNO_BUS_ERROR;
		else if (strstr(txt_response, ELM327_CAN_ERROR))
			elm.elm_errno = ELM_ERRNO_CAN_ERROR;
		else if (strstr(txt_response, ELM327_LINE_DATA_ERROR))
			elm.elm_errno = ELM_ERRNO_LINE_DATA_ERROR;
		else if (strstr(txt_response, ELM327_DATA_ERROR))
			elm.elm_errno = ELM_ERRNO_DATA_ERROR;
		else if (strstr(txt_response, ELM327_FEEDBACK_ERROR))
			elm.elm_errno = ELM_ERRNO_FEEDBACK_ERROR;
		else if (strstr(txt_response, ELM327_LINE_RX_ERROR))
			elm.elm_errno = ELM_ERRNO_LINE_RX_ERROR;
	} else if (strstr(txt_response, ELM327_OK))
		elm.elm_errno = ELM_ERRNO_OK;
	else if (strstr(txt_response, ELM327_INVALID))
		elm.elm_errno = ELM_ERRNO_INVALID;
	else if (strstr(txt_response, ELM327_ACT_ALERT))
		elm.elm_errno = ELM_ERRNO_ACT_ALERT;
	else if (strstr(txt_response, ELM327_BUFFER_FULL))
		elm.elm_errno = ELM_ERRNO_BUFFER_FULL;
	else if (strstr(txt_response, ELM327_BUS_BUSY))
		elm.elm_errno = ELM_ERRNO_BUS_BUSY;
	else if (strstr(txt_response, ELM327_LOW_POWER_ALERT))
		elm.elm_errno = ELM_ERRNO_LOW_POWER_ALERT;
	else if (strstr(txt_response, ELM327_LOW_VOLTAGE_RESET))
		elm.elm_errno = ELM_ERRNO_LOW_VOLTAGE_RESET;
	else if (strstr(txt_response, ELM327_NO_DATA))
		elm.elm_errno = ELM_ERRNO_NO_DATA;
	else if (strstr(txt_response, ELM327_STOPPED))
		elm.elm_errno = ELM_ERRNO_STOPPED;
	else if (strstr(txt_response, ELM327_NOCONN))
		elm.elm_errno = ELM_ERRNO_NOCONN;
	else if (strstr(txt_response, ELM327_SEARCHING))
		elm.elm_errno = ELM_ERRNO_SEARCHING;

	if (elm.elm_errno != ELM_ERRNO_OK) {
		fprintf(stderr, "[elm327plugin] elm_query: query was not successful. ELM_ERRNO: %d\n", elm.elm_errno);
		return EIO;
	}

	/*
	response_length = strlen(txt_response);
	printf("asked for \"%s\", response: \"", command);
	for (int i=0; i < response_length; i++) {
		if (isprint(txt_response[i])) printf("%c", txt_response[i]);
		else printf("\\x%02X", txt_response[i]);
	}
	printf("\"\n");
	*/

	// parse the response
	int res = sscanf(txt_response, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx",
					 bytes, bytes+1, bytes+2, bytes+3, bytes+4, bytes+5);

	if (res == EOF) {
		fprintf(stderr, "[elm327plugin] elm_query: string matching error ocurred\n");
		elm.elm_errno = ELM_ERRNO_INVALID;
		return EIO;
	} else if ((res >= 0) && (res <= 2)) {
		fprintf(stderr, "[elm327plugin] elm_query: unexpected ELM327 reply\n");
		elm.elm_errno = ELM_ERRNO_INVALID;
		return EIO;
	}

	// put the values returned from the ELM327 into the response
	*response = 0;
	for (int i = 2; i < res; i++)
		*response = (*response << 8) | bytes[i];

	return 0;
}

/// if the given pid is supported by the vehicle, returns true.
/// otherwise, returns false.
/// the supported PIDs are tested in elm_open, so this won't send anything
/// to the ELM327. 
bool elm_pid_supported(uint8_t pid) {
	if (pid == 0x00) return true;
	
	uint8_t pid_bank = (pid-1) >> 5;
	uint8_t pid_index = (pid-1) & 0x1F;
			pid_index = 0x1f - pid_index;

	return (elm.supported_pids[pid_bank] & (1 << pid_index)) && 1;
}

/// closes the underlying serial device
void elm_destroy() {
	if (elm.is_online) {
		cfsetispeed(&elm.tty, B0);
		cfsetospeed(&elm.tty, B0);
		
		close(elm.fd);
	}
}

/// Opens the serial device given through "serial_path" with the given baudrate,
///   and sets up the ELM327 at the other end for communication.
/// elm_command, elm_query, elm_pid_supported and elm_destroy can't be used
///   before elm_open was called.
int  elm_open(char *serial_path, int baudrate) {
	int ok;

	elm.timeout.tv_sec = 10;
	elm.timeout.tv_nsec = 0;
	elm.is_online = false;

	ok = access(serial_path, R_OK | W_OK);
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: process doesn't have access to serial device \"%s\": %s\n", serial_path, strerror(errno));
		return errno;
	}

	// Open serial device at given path
	elm.fd = open(serial_path, O_RDWR | O_NOCTTY | O_SYNC);
	if (elm.fd < 0) {
		fprintf(stderr, "[elm327plugin] elm_open: could not open serial device at \"%s\": %s\n", serial_path, strerror(errno));
		return errno;
	}

	// set the serial line attributes
	ok = tcgetattr(elm.fd, &elm.tty);
	if (ok != 0) goto error;

	/*
	elm.tty.c_cflag |=  (CLOCAL|CREAD);
	elm.tty.c_lflag &= ~(ICANON|ECHO|ECHOE|ECHOK|ECHONL|ISIG|IEXTEN);
	elm.tty.c_iflag &= ~(INLCR|IGNCR|ICRNL|IGNBRK|IUCLC|PARMRK|
							 INPCK|ISTRIP|IXON|IXOFF|IXANY);
	elm.tty.c_oflag &= ~(OPOST);
	
	elm.tty.c_cc[VMIN] = 0;
	elm.tty.c_cc[VTIME]= 0;
	*/

	// make raw terminal
	elm.tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
	elm.tty.c_oflag &= ~OPOST;
	elm.tty.c_cflag &= ~(CSIZE | PARENB);
	elm.tty.c_cflag |= CS8;
	elm.tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	
	elm.tty.c_cc[VMIN]  = 1;
	elm.tty.c_cc[VTIME] = 100;


	// set the baudrate
	speed_t serial_speed;
	switch (baudrate) {
		case 0: 	 serial_speed = B0; break;
		case 50: 	 serial_speed = B50; break;
		case 75: 	 serial_speed = B75; break;
		case 110:    serial_speed = B110; break;
		case 134:    serial_speed = B134; break;
		case 150:    serial_speed = B150; break;
		case 200:    serial_speed = B200; break;
		case 300:    serial_speed = B300; break;
		case 600:    serial_speed = B600; break;
		case 1200:   serial_speed = B1200; break;
		case 1800:   serial_speed = B1800; break;
		case 2400:   serial_speed = B2400; break;
		case 4800:   serial_speed = B4800; break;
		case 9600:   serial_speed = B9600; break;
		case 19200:  serial_speed = B19200; break;
		case 38400:  serial_speed = B38400; break;
		case 57600:  serial_speed = B57600; break;
		case 115200: serial_speed = B115200; break;
		case 230400: serial_speed = B230400; break;
		default:     serial_speed = B0; break;
	}
	if (serial_speed == B0) {
		fprintf(stderr, "[elm327plugin] elm_open: not a valid baudrate: %d\n", baudrate);
		return EINVAL;
	}

	ok = cfsetispeed(&(elm.tty), serial_speed);
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: could not set serial input speed: %s\n", strerror(ok));
		goto error;
	}

	ok = cfsetospeed(&(elm.tty), serial_speed);
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: could not set serial output speed: %s\n", strerror(ok));
		goto error;
	}

	ok = tcsetattr(elm.fd, TCSANOW, &(elm.tty));
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: could not set serial tty-config: %s\n", strerror(ok));
		goto error;
	}

	// create an fdset containing the serial device fd
	FD_ZERO(&elm.fdset);
	FD_SET(elm.fd, &elm.fdset);

	memset(elm.supported_pids, 0, sizeof(elm.supported_pids));

	// ELM327 is now connected.
	elm.is_online = true;

	// completely reset the ELM327
	ok = elm_command(ELM327_RESET, NULL, 0);
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: error resetting ELM327 using AT WS: %d\n", ok);
		goto error;
	}

	ok = elm_command(ELM327_ECHO_OFF, NULL, 0);
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: error setting ELM327 echo off using AT E0: %d\n", ok);
		goto error;
	}

	ok = elm_command(ELM327_LINEFEEDS_OFF, NULL, 0);
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: error setting ELM327 linefeeds off using AT L0\n", ok);
		goto error;
	}

	ok = elm_command(ELM327_VERSION, elm.version, sizeof(elm.version));
	if (ok != 0) {
		fprintf(stderr, "[elm327plugin] elm_open: error fetching ELM327 version using AT I: %d\n", ok);
		goto error;
	}

	// send a dummy message so the ELM can determine the cars protocol
	ok = elm_query(0x00, elm.supported_pids);
	if ((ok != 0) && (elm.elm_errno != ELM_ERRNO_SEARCHING)) {
		fprintf(stderr, "[elm327plugin] elm_open: error while querying PID 00 / searching for OBDII bus signal: %d\n", ok);
		ok = EIO;
		goto error;
	}

	// query supported pids
	for (int i = 0; i < 8; i++) {
		if (!elm_pid_supported(i*0x20)) break;
		ok = elm_query(i*0x20, &elm.supported_pids[i]);
		if (ok != 0) {
			fprintf(stderr, "[elm327plugin] elm_open: error while querying PID %02X: %d\n", i*0x20, ok);
			goto error;
		}
	}

	// output a list of supported PIDs
	printf("[elm327plugin] list of supported PIDs: ");
	for (uint8_t pid = 0;; pid++) {
		if (elm_pid_supported(pid))	printf("0x%02X, ", pid);
		if (pid == 0xFF) break;
	}
	printf("\n");
	
	return 0;

	error:
		elm.is_online = false;
		if (elm.fd >= 0) close(elm.fd); 
		return ok;
}

/*
 * pid-query priority queue
 * 
 * when the ELM327 plugin wants to know about a pid,
 * it has to queue this query in the pid-query queue (pidqq)
 * the queries are processed by the pidqq_processor, which runs on it's own thread.
 * pid queries with higher priorities are executed first.
 */
void pidqq_add(struct pidqq_element *element) {
	int index;

	// make queue larger if necessary
	if (pidqq_nelements == pidqq_size) {
        size_t before = pidqq_size*sizeof(struct pidqq_element);
        pidqq_size = pidqq_size*2;
        size_t after = pidqq_size*sizeof(struct pidqq_element);
        pidqq = realloc(pidqq, after);
        memset(pidqq + before, 0, after-before);
	}

	// find a nice place to insert the element
	for (index = 0; (pidqq[index].priority >= element->priority) && (index < pidqq_nelements); index++);

	// shift all elements above index one up
	for (int i = pidqq_nelements+1; i > index; i--)
		pidqq[i] = pidqq[i-1];
	
	pidqq[index] = *element;
	
	pidqq_nelements++;
}
void pidqq_remove(int index) {
	for (int i = index+1; (i < pidqq_nelements); i++)
		pidqq[i-1] = pidqq[i];
	pidqq[pidqq_nelements-1].priority = 0;

	pidqq_nelements--;
}
int  pidqq_findWithPid(uint8_t pid) {
    for (int i = 0; i < pidqq_nelements; i++)
        if (pidqq[i].pid == pid)
            return i;

    return -1;
}
void *run_pidqq_processor(void* arg) {
	ELM327PluginPIDQueryCompletionCallback completionCallback = NULL;
	struct pidqq_element element;
    uint8_t  pid;
    uint32_t result;

	printf("[elm327plugin] running pid query queue processor\n");

    while (pidqq_processor_shouldrun) {
		result = 0;

        pthread_mutex_lock(&pidqq_lock);
		while (!(pidqq[0].priority))
			pthread_cond_wait(&pidqq_added, &pidqq_lock);
		
		FlutterEngineTraceEventDurationBegin("pidqq_process");

		pid = pidqq[0].pid;
		pthread_mutex_unlock(&pidqq_lock);

		int ok = elm_query(pid, &result);
		
		pthread_mutex_lock(&pidqq_lock);
			element = pidqq[0];
			if ((element.priority) && (element.pid == pid)) {
				pidqq_remove(0);
				if (element.repeat) pidqq_add(&element);
			}
		pthread_mutex_unlock(&pidqq_lock);

		if ((element.priority) && (element.pid == pid) && (element.completionCallback)) {
			FlutterEngineTraceEventDurationBegin("pidqq completionCallback");
			element.completionCallback(element, result, elm.elm_errno);
			FlutterEngineTraceEventDurationEnd("pidqq completionCallback");
		}
		
		FlutterEngineTraceEventDurationEnd("pidqq_process");
    }

    return NULL;
}

/*****************
 * ELM327 plugin *
 *****************/
void ELM327Plugin_onPidQueryCompletion(struct pidqq_element query, uint32_t result, enum elm327plugin_errno elm_errno) {
	// channel object that will be returned to flutter.
	struct ChannelObject obj = {
		.codec = kStandardMethodCallResponse,
		.success = true,
		.stdresult = {
			.type = kFloat64,
			.float64_value = 0.0
		}
	};
	
	if (elm_errno == ELM_ERRNO_OK) {
		uint8_t pid = query.pid;

		switch (pid) {
			case OBDII_PID_ENGINE_RPM:
				obj.stdresult.float64_value = (double) result / (double) 4.0;
				break;
			case OBDII_PID_ENGINE_LOAD:
			case OBDII_PID_THROTTLE_POSITION:
				obj.stdresult.float64_value = result * 100.0 / 255.0;
				break;
			case OBDII_PID_ENGINE_COOLANT_TEMP:
			case OBDII_PID_INTAKE_AIR_TEMP:
				obj.stdresult.type = kInt32;
				obj.stdresult.int32_value = (int32_t) result - 40;
				break;
			case OBDII_PID_VEHICLE_SPEED:
				obj.stdresult.type = kInt32;
				obj.stdresult.int32_value = (int32_t) result;
				break;
			default: 
				break;
		}
	} else {
		obj.success = false;
		obj.errorcode = "queryfailed";
		obj.errormessage = "ELM327 PID query failed. Reason could be a timeout on the connection between Pi and ELM or between ELM and ECU, or something else.";
		obj.stderrordetails.type = kNull;
	}

	PlatformChannel_send(query.channel, &obj, kBinaryCodec, NULL, NULL);
}
int ELM327Plugin_onEventChannelListen(char *channel, uint8_t pid, FlutterPlatformMessageResponseHandle *handle) {
	printf("[elm327plugin] listener registered on event channel %s with pid 0x%02X\n", channel, pid);
	
	// check if pid is supported, if not, respond with an error envelope
	if (!elm_pid_supported(pid)) {
        return PlatformChannel_respondError(
            handle, kStandardMethodCallResponse,
            "notsupported",
            "The vehicle doesn't support the PID used for this channel.",
            NULL
        );
    }

	// copy the channel string.
	char *channel_copy = malloc(strlen(channel)+1);
	if (channel_copy == NULL) {
		fprintf(stderr, "[elm327plugin] could not allocate memory.\n");
		return ENOMEM;
	}
	strcpy(channel_copy, channel);

	// insert a new query into pidqq
	pthread_mutex_lock(&pidqq_lock);
		pidqq_add(&(struct pidqq_element) {
			.channel = channel_copy,
			.priority = 1,
			.pid = pid,
			.repeat = true,
			.completionCallback = ELM327Plugin_onPidQueryCompletion
		});
    pthread_mutex_unlock(&pidqq_lock);
	pthread_cond_signal(&pidqq_added);

	// respond with null
	return PlatformChannel_respond(handle, &(struct ChannelObject) {
		.codec = kStandardMethodCallResponse,
		.success = true,
		.stdresult = {.type = kNull}
	});
}
int ELM327Plugin_onEventChannelCancel(char *channel, uint8_t pid, FlutterPlatformMessageResponseHandle *handle) {
	// remove query from pidqq
	pthread_mutex_lock(&pidqq_lock);
		int index = pidqq_findWithPid(OBDII_PID_ENGINE_RPM);
		free(pidqq[index].channel);
		pidqq_remove(index);
    pthread_mutex_unlock(&pidqq_lock);

	// respond with null.
    return PlatformChannel_respond(handle, &(struct ChannelObject) {
        .codec = kStandardMethodCallResponse,
        .success = true,
        .stdresult = {.type = kNull}
    });
}
int ELM327Plugin_onReceive(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *handle) {
    bool isListen = false;
	if ((object->codec == kStandardMethodCall) && ((isListen = (strcmp(object->method, "listen") == 0)) || (strcmp(object->method, "cancel") == 0))) {
        uint8_t pid = (strcmp(channel, ELM327PLUGIN_RPM_CHANNEL) == 0)         ? OBDII_PID_ENGINE_RPM :
                      (strcmp(channel, ELM327PLUGIN_ENGINELOAD_CHANNEL) == 0)  ? OBDII_PID_ENGINE_LOAD :
                      (strcmp(channel, ELM327PLUGIN_COOLANTTEMP_CHANNEL) == 0) ? OBDII_PID_ENGINE_COOLANT_TEMP :
                      (strcmp(channel, ELM327PLUGIN_SPEED_CHANNEL) == 0)       ? OBDII_PID_VEHICLE_SPEED :
                      (strcmp(channel, ELM327PLUGIN_THROTTLE_CHANNEL) == 0)    ? OBDII_PID_THROTTLE_POSITION : 0;
        if (pid == 0) {
			fprintf(stderr, "[elm327plugin] ELM327Plugin_onReceive: unexpected event channel: \"%s\"\n", channel);
			return EINVAL;
		}

		if (elm.is_online) {
			if (isListen)   ELM327Plugin_onEventChannelListen(channel, pid, handle);
			else 			ELM327Plugin_onEventChannelCancel(channel, pid, handle);
		} else {
			return PlatformChannel_respondError(
				handle, kStandardMethodCallResponse,
				"noelm", "elm.is_online == false. No communication to ELM327 possible, or initialization failed.", NULL
			);
		}
    } else {
		return PlatformChannel_respondNotImplemented(handle);
	}
}

int ELM327Plugin_init(void) {
    int r = 0;
    
	// init the elm327
    r = elm_open(ELM327PLUGIN_DEVICE_PATH, ELM327PLUGIN_BAUDRATE);
	if (r != 0) {
		fprintf(stderr, "[elm327plugin] ELM327Plugin_init: ELM327 communication was not initialized successfully. elm327plugin won't supply any OBDII data. error code: %s\n", strerror(r));
	}

    // init pidquery queue
    pthread_mutex_init(&pidqq_lock, NULL);
    pidqq_size = 50;
    pidqq = calloc(pidqq_size, sizeof(struct pidqq_element));

	pidqq_processor_shouldrun = true;
    pthread_create(&pidqq_processor_thread, NULL, run_pidqq_processor, NULL);

    PluginRegistry_setReceiver(ELM327PLUGIN_CHANNEL, kStandardMethodCall, ELM327Plugin_onReceive);
	PluginRegistry_setReceiver(ELM327PLUGIN_RPM_CHANNEL, kStandardMethodCall, ELM327Plugin_onReceive);
	PluginRegistry_setReceiver(ELM327PLUGIN_ENGINELOAD_CHANNEL, kStandardMethodCall, ELM327Plugin_onReceive);
	PluginRegistry_setReceiver(ELM327PLUGIN_COOLANTTEMP_CHANNEL, kStandardMethodCall, ELM327Plugin_onReceive);
	PluginRegistry_setReceiver(ELM327PLUGIN_SPEED_CHANNEL, kStandardMethodCall, ELM327Plugin_onReceive);
	PluginRegistry_setReceiver(ELM327PLUGIN_THROTTLE_CHANNEL, kStandardMethodCall, ELM327Plugin_onReceive);
	
	return 0;
}
int ELM327Plugin_deinit(void) {
    pidqq_processor_shouldrun = false;
    pthread_join(pidqq_processor_thread, NULL);

	elm_destroy();

    free(pidqq);
	
	return 0;
}
