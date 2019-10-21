#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>
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

#include "../pluginregistry.h"
#include "elm327plugin.h"


struct elm327 elm;
pthread_mutex_t pidqq_mutex;
struct pidqq_element *pidqq = NULL;
size_t pidqq_size = 0;
volatile bool pidqq_processor_shouldrun = false;
pthread_t pidqq_processor_thread;

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

	assert(elm.is_online && "elm_command: ELM327 must be online");
	assert(cmd && "elm_command: command can't be NULL");
	
	ntransmit = strlen(cmd) + strlen(ELM327_EOC);

	// write cmd to line
	ok = pselect(elm.fd+1, NULL, &elm.fdset, NULL, &elm.timeout, NULL);
	if (ok > 0) {
		tcflush(elm.fd, TCIOFLUSH); 

		// why do we write byte per byte here?
		for (i=0; i < strlen(cmd); i++)
			count += write(elm.fd, (const void *) cmd+i, sizeof(char));
		
		for (i=0; i < strlen(ELM327_EOC); i++)
			count += write(elm.fd, (const void *) ELM327_EOC+i, sizeof(char));

		if (count != ntransmit) {
			fprintf(stderr, "could not write command to serial, written %d bytes, should be %ld\n", count, ntransmit);
			return EIO;
		}
	} else {
		fprintf(stderr, "elm connection timed out while writing, after %lus, %09luns\n", elm.timeout.tv_sec, elm.timeout.tv_nsec);
		elm.elm_errno = ELM_ERRNO_NOCONN;
		return EIO;
	}

	// wait for bytes to send
	tcdrain(elm.fd);

	// read response
	while (1) {
		ok = pselect(elm.fd+1, &elm.fdset, NULL, NULL, &elm.timeout, NULL);

		if (ok > 0) {
			ok = read(elm.fd, &charbuff, sizeof(char));

			if (isprint(charbuff))	printf("%c", charbuff);
			else 					printf("\\x%02x", charbuff);

			if (charbuff == '>') {
				if (response) response[i] = '\0';
				printf("\n");
				break;
			} else if (response) {
				response[i] = charbuff;
				i = (i+1) % length;
			}
		} else {
			printf("\n");
			fprintf(stderr, "ELM327 connection timed out while reading, after %lus, %09luns\n", elm.timeout.tv_sec, elm.timeout.tv_nsec);
			elm.elm_errno = ELM_ERRNO_NOCONN;
			return EIO;
		}
	}

	return 0;
}

/// queries the value of a pid
///   (uses elm_command for execution)
int  elm_query(uint8_t pid, uint32_t* response) {
	char txt_response[32], command[5];
	size_t response_length = 0;
	uint8_t bytes[6] = {0};
	int ok;

	sprintf(command, "01%02X", pid);

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
		fprintf(stderr, "elm_query: query was not successful. ELM_ERRNO: %d\n", elm.elm_errno);
		return EIO;
	}

	response_length = strlen(txt_response);
	
	// we need to remove all carriage returns so we can print out the response
	// remove all trailing carriage-returns
	for (int i = response_length-1; i>=0; i--) {
		if (txt_response[i] == 0x0D) {
			txt_response[i] = 0x00;
			response_length--;
		} else {
			break;
		}
	}

	// remove all remaining carriage-returns
	int n_move = 0;
	for (int i = 0; i <= response_length; i++)
		if (txt_response[i] == 0x0D) n_move++;
		else if (n_move) 			 txt_response[i-n_move] = txt_response[i];
	response_length -= n_move;

	printf("asked for \"%s\", response: \"%s\"\n", command, txt_response);

	// parse the response
	int res = sscanf(txt_response, "%2hhX%2hhX%2hhX%2hhX%2hhX%2hhX",
					 bytes, bytes+1, bytes+2, bytes+3, bytes+4, bytes+5);
	
	if (res == EOF) {
		fprintf(stderr, "elm_query: string matching error ocurred\n");
		return EIO;
	} else if ((res >= 0) && (res <= 2)) {
		fprintf(stderr, "elm_query: unexpected ELM327 reply\n");
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
	cfsetispeed(&elm.tty, B0);
	cfsetospeed(&elm.tty, B0);
	
	close(elm.fd);
}

/// Opens the serial device given through "serial_path" with the given baudrate,
///   and sets up the ELM327 at the other end for communication.
/// elm_command, elm_query, elm_pid_supported and elm_destroy can't be used
///   before elm_open was called.
int  elm_open(char *serial_path, int baudrate) {
	int ok;

	printf("Opening ELM327 at \"%s\"\n", serial_path);

	elm.timeout.tv_sec = 5;
	elm.timeout.tv_nsec = 0;
	elm.is_online = false;

	ok = access(serial_path, R_OK | W_OK);
	if (ok != 0) {
		fprintf(stderr, "Process doesn't have access to serial device \"%s\": %s\n", serial_path, strerror(errno));
		return errno;
	}

	// Open serial device at given path
	elm.fd = open(serial_path, O_RDWR | O_NOCTTY | O_NDELAY);
	if (elm.fd < 0) {
		fprintf(stderr, "Could not open serial device \"%s\": %s\n", serial_path, strerror(errno));
		return errno;
	}

	// set the serial line attributes
	ok = tcgetattr(elm.fd, &elm.tty);

	elm.tty.c_cflag |=  (CLOCAL|CREAD);
	elm.tty.c_lflag &= ~(ICANON|ECHO|ECHOE|ECHOK|ECHONL|ISIG|IEXTEN);
	elm.tty.c_iflag &= ~(INLCR|IGNCR|ICRNL|IGNBRK|IUCLC|PARMRK|
							 INPCK|ISTRIP|IXON|IXOFF|IXANY);
	elm.tty.c_oflag &= ~(OPOST);
	
	elm.tty.c_cc[VMIN] = 0;
	elm.tty.c_cc[VTIME]= 0;


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
		fprintf(stderr, "Not a valid baudrate: %d\n", baudrate);
		return EINVAL;
	}

	cfsetispeed(&(elm.tty), serial_speed);
	cfsetospeed(&(elm.tty), serial_speed);

	tcsetattr(elm.fd, TCSANOW, &(elm.tty));

	// create an fdset containing the serial device fd
	FD_ZERO(&elm.fdset);
	FD_SET(elm.fd, &elm.fdset);

	// completely reset the ELM327
	printf("elm ATWS\n");
	elm.is_online = true;
	ok = elm_command(ELM327_RESET, NULL, 0);
	if (ok != 0) return ok;

	printf("elm ATI\n");
	ok = elm_command(ELM327_VERSION, elm.version, sizeof(elm.version));
	if (ok != 0) return ok;

	printf("elm AT E0\n");
	ok = elm_command(ELM327_ECHO_OFF, NULL, 0);
	if (ok != 0) return ok;

	printf("elm AT L0\n");
	ok = elm_command(ELM327_LINEFEEDS_OFF, NULL, 0);
	if (ok != 0) return ok;

	// send a dummy message so the ELM can determine the cars protocol
	ok = elm_query(0x00, elm.supported_pids);
	if ((ok != 0) && (elm.elm_errno != ELM_ERRNO_SEARCHING)) return EIO;

	// query supported pids
	for (int i = 0; i < 8; i++) {
		printf("is PID 0x%02X supported? %s\n", i*0x20, elm_pid_supported(i*0x20) ? "yes" : "no");
		ok = elm_query(i*0x20, &elm.supported_pids[i]);
		if (ok != 0) return ok;
	}

	// output a list of supported PIDs
	printf("list of supported PIDs: ");
	for (uint8_t pid = 0; pid <= 0xFF; pid++)
		if (elm_pid_supported(pid))
			printf("0x%02X, ", pid);
	
	printf("\n");
}

/*
 * pid-query priority queue
 * 
 * when the ELM327 plugin wants to know about a pid,
 * it has to queue this query in the pid-query queue (pidqq)
 * the queries are processed by the pidqq_processor, which runs on it's own thread.
 * pid queries with higher priorities are executed first.
 */
int  pidqq_lastIndex(void) {
    int i = 0;
    while ((i > 0) && (pidqq[i].priority == 0))
        i--;
    
    return i;
}
void pidqq_add(struct pidqq_element *element) {
    int lastIndex = -1;
    int i = 0;

    while ((i < pidqq_size) && (pidqq[i].priority != 0) && (pidqq[i].priority > element->priority))
        i++;

    if ((i == pidqq_size) || ((lastIndex = pidqq_lastIndex()) == pidqq_size-1)) {
        // make queue larger
        size_t before = pidqq_size*sizeof(struct pidqq_element);
        pidqq_size = pidqq_size*2;
        size_t after = pidqq_size*sizeof(struct pidqq_element);
        pidqq = realloc(pidqq, after);
        memset(pidqq + before, 0, after-before);
    }

    if (pidqq[i].priority == 0) {
        // insert at last element
        pidqq[i] = *element;
    } else {
        if (lastIndex == -1) lastIndex = pidqq_lastIndex();

        // shift all elements from i to lastIndex one up
        for (int j = lastIndex+1; j>i; j--)
            pidqq[j] = pidqq[j-1];
        
        // insert element at i
        pidqq[i] = *element;
    }
}
void pidqq_remove(int index) {
    int lastIndex = pidqq_lastIndex();
    pidqq[index].priority = 0;

    int i;
    for (i = index+1; (i < pidqq_size) && (pidqq[i].priority != 0); i++) {
        pidqq[i-1] = pidqq[i];
    }
    pidqq[i-1].priority = 0;
}
int  pidqq_findWithPid(uint8_t pid) {
    for (int i = 0; (i < pidqq_size) && (pidqq[i].priority != 0); i++)
        if (pidqq[i].pid == pid)
            return i;

    return -1;
}
void *run_pidqq_processor(void* arg) {
    uint8_t  pid;
    uint32_t response;

    while (pidqq_processor_shouldrun) {
        pthread_mutex_lock(&pidqq_mutex);
        if (pidqq[0].priority) {
            pid = pidqq[0].pid;
            
            pthread_mutex_unlock(&pidqq_mutex);
            response = 0;
            elm_query(pid, &response);
            
            pthread_mutex_lock(&pidqq_mutex);
            if ((pidqq[0].priority) && (pidqq[0].pid == pid)) {
                struct pidqq_element e = pidqq[0];
                if (pidqq[0].repeat)   pidqq_add(&e);

                pidqq_remove(0);
                pthread_mutex_unlock(&pidqq_mutex);

                e.completionCallback(e, response);
            } else {
                pidqq_remove(0);
                pthread_mutex_unlock(&pidqq_mutex);
            }
        } else {
            pthread_mutex_unlock(&pidqq_mutex);
        }
    }

    return NULL;
}

/*****************
 * ELM327 plugin *
 *****************/
void ELM327Plugin_onPidQueryCompletion(struct pidqq_element query, uint32_t result) {
	struct ChannelObject obj = {
		.codec = kStandardMessageCodec,
		.stdmsgcodec_value = {
			.type = kFloat64,
			.float64_value = 0.0
		}
	};
	uint8_t pid = query.pid;

	switch (pid) {
		case OBDII_PID_ENGINE_RPM:
			obj.stdmsgcodec_value.float64_value = result / 4.0;
			break;
		case OBDII_PID_ENGINE_LOAD:
		case OBDII_PID_THROTTLE_POSITION:
			obj.stdmsgcodec_value.float64_value = result * 100.0 / 255.0;
			break;
		case OBDII_PID_ENGINE_COOLANT_TEMP:
			obj.stdmsgcodec_value.type = kInt32;
			obj.stdmsgcodec_value.int32_value = (int32_t) result - 40;
			break;
		case OBDII_PID_VEHICLE_SPEED:
			obj.stdmsgcodec_value.type = kInt32;
			obj.stdmsgcodec_value.int32_value = (int32_t) result;
			break;
		default:
			break;
	}

    PlatformChannel_send(query.channel, &obj, kStandardMessageCodec, NULL, NULL);
}
int ELM327Plugin_onEventChannelListen(char *channel, uint8_t pid, FlutterPlatformMessageResponseHandle *handle) {
    pthread_mutex_lock(&pidqq_mutex);

    pidqq_add(&(struct pidqq_element) {
        .priority = 1,
        .pid = pid,
        .repeat = true,
        .completionCallback = ELM327Plugin_onPidQueryCompletion
    });

    pthread_mutex_unlock(&pidqq_mutex);

    if (elm_pid_supported(pid)) {
        PlatformChannel_respond(handle, &(struct ChannelObject) {
            .codec = kStandardMethodCallResponse,
            .success = true,
            .stdresult = {.type = kNull}
        });
    } else {
        PlatformChannel_respondError(
            handle, kStandardMethodCallResponse,
            "notsupported",
            "The vehicle doesn't support the PID used for this channel.",
            NULL
        );
    }
}
int ELM327Plugin_onEventChannelCancel(char *channel, uint8_t pid, FlutterPlatformMessageResponseHandle *handle) {
    pthread_mutex_lock(&pidqq_mutex);

    pidqq_remove(pidqq_findWithPid(OBDII_PID_ENGINE_RPM));
    
    pthread_mutex_unlock(&pidqq_mutex);

    PlatformChannel_respond(handle, &(struct ChannelObject) {
        .codec = kStandardMethodCallResponse,
        .success = true,
        .stdresult = {.type = kNull}
    });
}
int ELM327Plugin_onReceive(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *handle) {
    if (object->codec == kStandardMethodCall) {
        uint8_t pid = (strcmp(channel, ELM327PLUGIN_RPM_CHANNEL) == 0)         ? OBDII_PID_ENGINE_RPM :
                      (strcmp(channel, ELM327PLUGIN_ENGINELOAD_CHANNEL) == 0)  ? OBDII_PID_ENGINE_LOAD :
                      (strcmp(channel, ELM327PLUGIN_COOLANTTEMP_CHANNEL) == 0) ? OBDII_PID_ENGINE_COOLANT_TEMP :
                      (strcmp(channel, ELM327PLUGIN_SPEED_CHANNEL) == 0)       ? OBDII_PID_VEHICLE_SPEED :
                      (strcmp(channel, ELM327PLUGIN_THROTTLE_CHANNEL) == 0)    ? OBDII_PID_THROTTLE_POSITION : 0;
        assert((pid != 0) && "unexpected event channel");

        if (strcmp(object->method, "listen") == 0)      ELM327Plugin_onEventChannelListen(channel, pid, handle);
        else if (strcmp(object->method, "cancel") == 0) ELM327Plugin_onEventChannelCancel(channel, pid, handle);
    }

    return PlatformChannel_respondNotImplemented(handle);
}

int ELM327Plugin_init(void) {
    int r = 0;
    
	// init the elm327
    r = elm_open(ELM327PLUGIN_DEVICE_PATH, 9600);

    // init pidquery queue
    pthread_mutex_init(&pidqq_mutex, NULL);
    pidqq_size = 50;
    pidqq = calloc(pidqq_size, sizeof(struct pidqq_element));

    pthread_create(&pidqq_processor_thread, NULL, run_pidqq_processor, NULL);

    PluginRegistry_setReceiver(ELM327PLUGIN_CHANNEL, kStandardMethodCall, ELM327Plugin_onReceive);
}
int ELM327Plugin_deinit(void) {
	elm_destroy();

    pidqq_processor_shouldrun = false;
    pthread_join(pidqq_processor_thread, NULL);

    free(pidqq);
}