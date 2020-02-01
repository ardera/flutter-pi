#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/spi/spidev.h>

#include <flutter-pi.h>
#include <platformchannel.h>
#include <pluginregistry.h>

#include "spi_plugin.h"

enum spi_task_type {
    kSpiTaskClose,
    kSpiTaskRdMode,
    kSpiTaskWrMode,
    kSpiTaskWrBitsPerWord,
    kSpiTaskRdBitsPerWord,
    kSpiTaskWrMaxSpeedHz,
    kSpiTaskRdMaxSpeedHz,
    kSpiTaskTransmit
};

struct spi_task {
    enum spi_task_type type;
    union {
        uint8_t mode;
        uint8_t bits;
        uint64_t speed;
        struct spi_ioc_transfer transfer;
    };
    FlutterPlatformMessageResponseHandle *responsehandle;
};

struct spi_thread {
    int fd;
    bool has_task;
    struct spi_task task;
    pthread_mutex_t mutex;
    pthread_cond_t  task_added;
};

#define SPI_THREAD_INITIALIZER \
    ((struct spi_thread) { \
        .fd = -1, .has_task = false, \
        .mutex = PTHREAD_MUTEX_INITIALIZER, .task_added = PTHREAD_COND_INITIALIZER \
    })

struct {
    struct spi_thread *threads;
    pthread_mutex_t threadlist_mutex;
    size_t size_threads;
    size_t num_threads;
} spi_plugin = {
    .threads = NULL,
    .threadlist_mutex = PTHREAD_MUTEX_INITIALIZER,
    .size_threads = 0,
    .num_threads = 0
};

void *SPIPlugin_run_spi_thread(struct spi_thread *thread) {
    bool running = true;
    int  fd = thread->fd;
    int  err = 0;
    int  ok;

    while (running) {
        pthread_mutex_lock(&thread->mutex);
        while (!thread->has_task)
            pthread_cond_wait(&thread->task_added, &thread->mutex);
        
        switch (thread->task.type) {
            case kSpiTaskClose:
                ok = close(fd);
                if (ok == -1) {
                    err = errno;
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                running = false;
                thread->fd = -1;

                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {.codec = kStandardMethodCallResponse, .success = true, .stdresult = {.type = kNull}}
                );
                break;

            case kSpiTaskRdMode:
                ok = ioctl(fd, SPI_IOC_RD_MODE, &thread->task.mode);
                if (ok == -1) {
                    err = errno;
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                int32_t result = thread->task.mode;
                
                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {
                        .codec = kStandardMethodCallResponse, .success = true,
                        .stdresult = {.type = kInt32, .int32_value = result}
                    }
                );
                break;

            case kSpiTaskWrMode:
                ok = ioctl(fd, SPI_IOC_WR_MODE, &thread->task.mode);
                if (ok == -1) {
                    err = errno;
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {.codec = kStandardMethodCallResponse, .success = true, .stdresult = {.type = kNull}}
                );
                break;

            case kSpiTaskWrBitsPerWord:
                ok = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &thread->task.bits);
                if (ok == -1) {
                    err = errno;
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {.codec = kStandardMethodCallResponse, .success = true, .stdresult = {.type = kNull}}
                );
                break;

            case kSpiTaskRdBitsPerWord:
                ok = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &thread->task.bits);
                if (ok == -1) {
                    err = errno;
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                int32_t result = thread->task.bits;

                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {
                        .codec = kStandardMethodCallResponse, .success = true,
                        .stdresult = {.type = kInt32, .int32_value = result}
                    }
                );
                break;

            case kSpiTaskWrMaxSpeedHz:
                ok = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &thread->task.speed);
                if (ok == -1) {
                    err = errno;
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {.codec = kStandardMethodCallResponse, .success = true, .stdresult = {.type = kNull}}
                );
                break;

            case kSpiTaskRdMaxSpeedHz:
                ok = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &thread->task.speed);
                if (ok == -1) {
                    err = errno;
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                int32_t result = thread->task.speed;

                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {
                        .codec = kStandardMethodCallResponse, .success = true,
                        .stdresult = {.type = kInt64, .int64_value = result}
                    }
                );
                break;

            case kSpiTaskTransmit:
                ok = ioctl(fd, SPI_IOC_MESSAGE(1), thread->task.transfer);
                if (ok == -1) {
                    err = errno;
                    free(thread->task.transfer.tx_buf);
                    pthread_mutex_unlock(&thread->mutex);
                    break;
                }

                size_t   len = thread->task.transfer.len;
                uint8_t *buf = thread->task.transfer.rx_buf;

                pthread_mutex_unlock(&thread->mutex);
                PlatformChannel_respond(
                    thread->task.responsehandle,
                    &(struct ChannelObject) {
                        .codec = kStandardMethodCallResponse, .success = true,
                        .stdresult = {
                            .type = kUInt8Array,
                            .size = len,
                            .uint8array = buf
                        }
                    }
                );

                free(buf);

                break;

            default:
                break;
        }
        
        thread->has_task = false;
        if (err != 0) {
            PlatformChannel_respondError(
                thread->task.responsehandle,
                kStandardMethodCallResponse,
                "nativeerror",
                strerror(err),
                NULL
            );
            err = 0;
        }
    }
}

struct spi_thread *SPIPlugin_get_thread(const int fd) {
    pthread_mutex_lock(&spi_plugin.threadlist_mutex);

    for (int i = 0; i < spi_plugin.num_threads; i++) {
        if (spi_plugin.threads[i].fd == fd)
            return &spi_plugin.threads[i];
    }

    pthread_mutex_unlock(&spi_plugin.threadlist_mutex);
    return NULL;
}

struct spi_thread *SPIPlugin_new_thread(const int fd) {
    struct spi_thread *thread;
    int ok;

    pthread_mutex_lock(&spi_plugin.threadlist_mutex);

    thread = &spi_plugin.threads[spi_plugin.num_threads++];
    thread->fd = fd;

    pthread_mutex_unlock(&spi_plugin.threadlist_mutex);

    ok = pthread_create(NULL, NULL, SPIPlugin_run_spi_thread, thread);
    if (ok == -1) return errno;
    
    return 0;
}

int SPIPlugin_assign_task(const int fd, const struct spi_task *const task) {
    struct spi_thread *thread;
    int ok;

    thread = SPIPlugin_get_thread(fd);
    if (!thread) {
        return EBADF;
    }
    
    ok = pthread_mutex_trylock(&thread->mutex);
    if (ok == -1) {
        return errno;
    } else if (ok == 0) {
        thread->task = *task;
        thread->has_task = true;
        pthread_mutex_unlock(&thread->mutex);
        pthread_cond_signal(&thread->task_added);
        return 0;
    }
}
[]
int SPIPlugin_onReceive(char *channel, struct ChannelObject *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct StdMsgCodecValue *temp;
    struct spi_thread *thread;
    struct spi_task task;
    bool has_task = false;
    int ok, fd;
    
    if STREQ("open", object->method) {
        if (object->stdarg.type != kString) {
            errormsg = "expected string as argument";
            goto respond_error;
        }

        char *path = object->stdarg.string_value;

        fd = open(path, O_RDWR);
        if (fd == -1) {
            errorcode = "nativerror";
            errormsg =  strerror(errno);
            goto respond_error;
        }

        ok = SPIPlugin_new_thread(fd);
        if (ok != 0) {
            errorcode = "nativerror";
            errormsg = strerror(ok);
            goto respond_error;
        }

    } else if STREQ("setMode", object->method) {
        if ((object->stdarg.type == kList) && (object->stdarg.size == 2) &&
            (object->stdarg.list[0].type == kInt32) && (object->stdarg.list[1].type == kInt32)) {
            fd = object->stdarg.list[0].int32_value;
            task.mode = object->stdarg.int32_value;
        } else if ((object->stdarg.type == kInt32Array) && (object->stdarg.size == 2)) {
            fd = object->stdarg.int32array[0];
            task.mode = object->stdarg.int32array[1];
        } else {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "invalidargument",
                "expected list containing two int32's or an int32 array with size 2 as argument",
                NULL
            );
        }

        task.type = kSpiTaskWrMode;
        has_task = true;
    } else if STREQ("getMode", object->method) {
        if (object->stdarg.type == kInt32) {
            fd = object->stdarg.int32_value;
        } else  {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "invalidargument",
                "expected int32 as argument",
                NULL
            );
        }

        task.type = kSpiTaskRdMode;
        has_task = true;
    } else if STREQ("setMaxSpeed", object->method) {
        if ((object->stdarg.type == kList) && (object->stdarg.size == 2) &&
            (object->stdarg.list[0].type == kInt32) && (object->stdarg.list[1].type == kInt32)) {
            fd = object->stdarg.list[0].int32_value;
            task.speed = object->stdarg.int32_value;
        } else if ((object->stdarg.type == kInt32Array) && (object->stdarg.size == 2)) {
            fd = object->stdarg.int32array[0];
            task.speed = object->stdarg.int32array[1];
        } else {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "invalidargument",
                "expected list containing two int32's or an int32 array with size 2 as argument",
                NULL
            );
        }
        
        task.type = kSpiTaskWrMaxSpeedHz;
        has_task = true;
    } else if STREQ("getMaxSpeed", object->method) {
        if (object->stdarg.type == kInt32) {
            fd = object->stdarg.int32_value;
        } else  {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "invalidargument",
                "expected int32 as argument",
                NULL
            );
        }

        task.type = kSpiTaskRdMaxSpeedHz;
        has_task = true;
    } else if STREQ("setWordSize", object->method) {
        if ((object->stdarg.type == kList) && (object->stdarg.size == 2) &&
            (object->stdarg.list[0].type == kInt32) && (object->stdarg.list[1].type == kInt32)) {
            fd = object->stdarg.list[0].int32_value;
            task.bits = object->stdarg.int32_value;
        } else if ((object->stdarg.type == kInt32Array) && (object->stdarg.size == 2)) {
            fd = object->stdarg.int32array[0];
            task.bits = object->stdarg.int32array[1];
        } else {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "invalidargument",
                "expected list containing two int32's or an int32 array with size 2 as argument",
                NULL
            );
        }

        task.type = kSpiTaskWrBitsPerWord;
        has_task = true;
    } else if STREQ("getWordSize", object->method) {
        if (object->stdarg.type == kInt32) {
            fd = object->stdarg.int32_value;
        } else  {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "invalidargument",
                "expected int32 as argument",
                NULL
            );
        }

        task.type = kSpiTaskRdBitsPerWord;
        has_task = true;
    } else if STREQ("transmit", object->method) {
        bool is_valid = false;

        if (object->stdarg.type == kMap) {
            is_valid = true;

            temp = stdmap_get_str(&object->stdarg, "fd");
            if (temp && (temp->type == kInt32)) {
                fd = temp->int32_value;
            } else {
                is_valid = false;
            }

            temp = stdmap_get_str(&object->stdarg, "speed");
            if ((!temp) || (temp && temp->type == kInt32)) {
                task.transfer.speed_hz = temp ? temp->int32_value : 0;
            } else {
                is_valid = false;
            }

            temp = stdmap_get_str(&object->stdarg, "delay");
            if ((!temp) || (temp && temp->type == kInt32)) {
                task.transfer.delay_usecs = temp ? temp->int32_value : 0;
            } else {
                is_valid = false;
            }

            temp = stdmap_get_str(&object->stdarg, "wordSize");
            if ((!temp) || (temp && temp->type == kInt32)) {
                task.transfer.bits_per_word = temp ? temp->int32_value : 0;
            } else {
                is_valid = false;
            }

            temp = stdmap_get_str(&object->stdarg, "csChange");
            if (!temp || (temp && (temp->type == kTrue || temp->type == kFalse))) {
                task.transfer.cs_change = temp && temp->type == kTrue;
            } else {
                is_valid = false;
            }

            if (is_valid) {
                temp = stdmap_get_str(&object->stdarg, "buffer");
                if (temp && temp->type == kUInt8Array) {
                    task.transfer.len = temp->size;
                    task.transfer.tx_buf = malloc(temp->size);
                    task.transfer.rx_buf = task.transfer.tx_buf;

                    memcpy(task.transfer.tx_buf, temp->uint8array, temp->size);
                } else {
                    is_valid = false;
                }
            }
        }

        if (!is_valid) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "invalidargument",
                "",
                NULL
            );
        }

        task.type = kSpiTaskTransmit;
        has_task = true;
    } else if STREQ("close", object->method) {
        task.type = kSpiTaskClose;
        has_task = true;
    }

    if (has_task) {
        ok = SPIPlugin_assign_task(fd, &task);
        if (ok == 0) {
            return;
        } else if (ok == EBUSY) {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "busy",
                "a different task is running on the fd already",
                NULL
            );
        } else {
            return PlatformChannel_respondError(
                responsehandle,
                kStandardMethodCallResponse,
                "nativeerror",
                strerror(ok),
                NULL
            );
        }
    }

    return PlatformChannel_respondNotImplemented(responsehandle);
}

int SPIPlugin_init(void) {
    printf("[spi-plugin] init.\n");

    spi_plugin.size_threads = 1;
    spi_plugin.threads = calloc(spi_plugin.size_threads, sizeof(struct spi_thread));

    for (int i = 0; i < spi_plugin.size_threads; i++)
        spi_plugin.threads[i] = SPI_THREAD_INITIALIZER;

    PluginRegistry_setReceiver(SPI_PLUGIN_METHOD_CHANNEL, kStandardMethodCall, SPIPlugin_onReceive);
    return 0;
}

int SPIPlugin_deinit(void) {
    printf("[spi-plugin] deinit.\n");
    return 0;
}