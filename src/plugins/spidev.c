#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/spi/spidev.h>

#include <flutter-pi.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include <plugins/spidev.h>

enum spidevp_task_type {
    kSpiTaskClose,
    kSpiTaskRdMode,
    kSpiTaskWrMode,
    kSpiTaskWrBitsPerWord,
    kSpiTaskRdBitsPerWord,
    kSpiTaskWrMaxSpeedHz,
    kSpiTaskRdMaxSpeedHz,
    kSpiTaskTransmit
};

struct spidevp_task {
    enum spidevp_task_type type;
    union {
        uint8_t mode;
        uint8_t bits;
        uint64_t speed;
        struct spi_ioc_transfer transfer;
    };
    FlutterPlatformMessageResponseHandle *responsehandle;
};

struct spidevp_thread {
    int fd;
    bool has_task;
    struct spidevp_task task;
    pthread_t thread;
    pthread_mutex_t task_mutex;
    pthread_cond_t  task_added;
};

#define SPI_THREAD_INITIALIZER \
    ((struct spidevp_thread) { \
        .fd = -1, .has_task = false, \
        .task_mutex = PTHREAD_MUTEX_INITIALIZER, .task_added = PTHREAD_COND_INITIALIZER \
    })

struct {
    struct spidevp_thread **threads;
    pthread_mutex_t threads_mutex;
    size_t size_threads;
    size_t num_threads;
} spi_plugin = {
    .threads = NULL,
    .threads_mutex = PTHREAD_MUTEX_INITIALIZER,
    .size_threads = 0
};

static void *spidevp_run_spi_thread(void *_thread) {
    FlutterPlatformMessageResponseHandle *responsehandle;
    struct spidevp_thread *thread = (struct spidevp_thread*) _thread;
    int32_t result;
    bool running = true;
    int  ok, err = 0, fd = thread->fd;

    while (running) {
        pthread_mutex_lock(&thread->task_mutex);
        while (!thread->has_task) {
            pthread_cond_wait(&thread->task_added, &thread->task_mutex);
        }

        responsehandle = thread->task.responsehandle;
        switch (thread->task.type) {
            case kSpiTaskClose:
                ok = close(fd);
                if (ok == -1) {
                    err = errno;
                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                running = false;
                thread->has_task = false;
                thread->fd = -1;
                pthread_mutex_unlock(&thread->task_mutex);
                platch_respond_success_std(responsehandle, NULL);
                break;

            case kSpiTaskRdMode:
                ok = ioctl(fd, SPI_IOC_RD_MODE, &thread->task.mode);
                if (ok == -1) {
                    err = errno;
                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                result = thread->task.mode;
                
                thread->has_task = false;
                pthread_mutex_unlock(&thread->task_mutex);
                ok = platch_respond_success_std(responsehandle, &STDINT32(result));
                
                break;

            case kSpiTaskWrMode:
                ok = ioctl(fd, SPI_IOC_WR_MODE, &thread->task.mode);
                if (ok == -1) {
                    err = errno;
                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                thread->has_task = false;
                pthread_mutex_unlock(&thread->task_mutex);
                platch_respond_success_std(responsehandle, NULL);
                break;

            case kSpiTaskWrBitsPerWord:
                ok = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &thread->task.bits);
                if (ok == -1) {
                    err = errno;
                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                thread->has_task = false;
                pthread_mutex_unlock(&thread->task_mutex);
                platch_respond_success_std(responsehandle, NULL);
                break;

            case kSpiTaskRdBitsPerWord:
                ok = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &thread->task.bits);
                if (ok == -1) {
                    err = errno;
                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                result = thread->task.bits;

                thread->has_task = false;
                pthread_mutex_unlock(&thread->task_mutex);
                platch_respond_success_std(responsehandle, &STDINT32(result));
                break;

            case kSpiTaskWrMaxSpeedHz:
                ok = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &thread->task.speed);
                if (ok == -1) {
                    err = errno;
                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                thread->has_task = false;
                pthread_mutex_unlock(&thread->task_mutex);
                platch_respond_success_std(responsehandle, NULL);
                break;

            case kSpiTaskRdMaxSpeedHz:
                ok = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &thread->task.speed);
                if (ok == -1) {
                    err = errno;
                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                result = thread->task.speed;

                thread->has_task = false;
                pthread_mutex_unlock(&thread->task_mutex);
                platch_respond_success_std(responsehandle, &STDINT64(result));
                break;

            case kSpiTaskTransmit: ;
                size_t   len = thread->task.transfer.len;
                uint8_t *buf = (void*) ((uintptr_t) thread->task.transfer.rx_buf);

                ok = ioctl(fd, SPI_IOC_MESSAGE(1), &thread->task.transfer);
                if (ok == -1) {
                    err = errno;
                    free(buf);

                    thread->has_task = false;
                    pthread_mutex_unlock(&thread->task_mutex);
                    break;
                }

                thread->has_task = false;
                pthread_mutex_unlock(&thread->task_mutex);
                platch_respond_success_std(
                    responsehandle,
                    &(struct std_value) {
                        .type = kStdUInt8Array,
                        .size = len,
                        .uint8array = buf
                    }
                );

                free(buf);

                break;

            default:
                break;
        }
        
        if (err != 0) {
            platch_respond_native_error_std(responsehandle, err);
            err = 0;
        }
    }

    return NULL;
}

static struct spidevp_thread *spidevp_get_thread(const int fd) {
    pthread_mutex_lock(&spi_plugin.threads_mutex);

    for (int i = 0; i < spi_plugin.size_threads; i++) {
        if (spi_plugin.threads[i]->fd == fd) {
            struct spidevp_thread *result = spi_plugin.threads[i];
            pthread_mutex_unlock(&spi_plugin.threads_mutex);
            return result;
        }
    }

    pthread_mutex_unlock(&spi_plugin.threads_mutex);
    return NULL;
}

static int spidevp_new_thread(const int fd, struct spidevp_thread **thread_out) {
    struct spidevp_thread *thread;
    int ok;

    pthread_mutex_lock(&spi_plugin.threads_mutex);
    
    thread = NULL;
    for (int i=0; i < spi_plugin.size_threads; i++) {
        if (spi_plugin.threads[i]->fd == -1) {
            thread = spi_plugin.threads[i];
            break;
        }
    }
    
    if (thread == NULL) {
        size_t old = spi_plugin.size_threads;
        size_t new = old*2;
        
        spi_plugin.threads = realloc(spi_plugin.threads, new * sizeof(struct spidevp_thread));
        spi_plugin.size_threads = new;

        for (int i=old; i < spi_plugin.size_threads; i++) {
            spi_plugin.threads[i] = malloc(sizeof(struct spidevp_thread)); 
            *(spi_plugin.threads[i]) = SPI_THREAD_INITIALIZER;
        }

        thread = spi_plugin.threads[old];
    }

    thread->fd = fd;

    pthread_mutex_unlock(&spi_plugin.threads_mutex);

    ok = pthread_create(&thread->thread, NULL, spidevp_run_spi_thread, thread);
    if (ok == -1) return errno;

    *thread_out = thread;
    
    return 0;
}

static int spidevp_assign_task(const int fd, const struct spidevp_task *const task) {
    struct spidevp_thread *thread;
    int ok;

    thread = spidevp_get_thread(fd);
    if (!thread) {
        return EBADF;
    }
    
    ok = pthread_mutex_trylock(&thread->task_mutex);
    if (ok == -1) {
        return errno;
    } else if (ok == 0) {
        if (thread->fd == -1) {
            return EBADF;
        } else {
            thread->task = *task;
            thread->has_task = true;
            pthread_mutex_unlock(&thread->task_mutex);
            pthread_cond_signal(&thread->task_added);
        }
    }

    return 0;
}

static int spidevp_open(struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct spidevp_thread *thread;
    char *path;
    int fd, ok;
    
    if (object->std_arg.type == kStdString) {
        path = object->std_arg.string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a string."
        );
    }

    fd = open(path, O_RDWR);
    if (fd == -1) {
        return platch_respond_native_error_std(responsehandle, errno);
    }

    ok = spidevp_new_thread(fd, &thread);
    if (ok != 0) {
        return platch_respond_native_error_std(responsehandle, ok);
    }

    return platch_respond_success_std(responsehandle, &STDINT32(fd));
}

static int spidevp_on_receive(char *channel, struct platch_obj *object, FlutterPlatformMessageResponseHandle *responsehandle) {
    struct std_value *temp;
    struct spidevp_thread *thread;
    struct spidevp_task task = {0};
    bool has_task = false;
    int ok, fd;
    
    if STREQ("open", object->method) {
        return spidevp_open(object, responsehandle);
    } else if STREQ("setMode", object->method) {
        if (STDVALUE_IS_SIZED_LIST(object->std_arg, 2) && STDVALUE_IS_INT(object->std_arg.list[0])
                                                      && STDVALUE_IS_INT(object->std_arg.list[1])) {
            fd = STDVALUE_AS_INT(object->std_arg.list[0]);
            task.mode = STDVALUE_AS_INT(object->std_arg.list[1]);
        } else {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg` to be a List<int> with size 2."
            );
        }
        
        task.type = kSpiTaskWrMode;
        has_task = true;
    } else if STREQ("getMode", object->method) {
        if (STDVALUE_IS_INT(object->std_arg)) {
            fd = STDVALUE_AS_INT(object->std_arg);
        } else  {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg` to be an integer."
            );
        }

        task.type = kSpiTaskRdMode;
        has_task = true;
    } else if STREQ("setMaxSpeed", object->method) {
        if (STDVALUE_IS_SIZED_LIST(object->std_arg, 2) && STDVALUE_IS_INT(object->std_arg.list[0])
                                                      && STDVALUE_IS_INT(object->std_arg.list[1])) {
            fd = STDVALUE_AS_INT(object->std_arg.list[0]);
            task.speed = STDVALUE_AS_INT(object->std_arg.list[1]);
        } else {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg` to be a List<int> with size 2."
            );
        }
        
        task.type = kSpiTaskWrMaxSpeedHz;
        has_task = true;
    } else if STREQ("getMaxSpeed", object->method) {
        if (STDVALUE_IS_INT(object->std_arg)) {
            fd = STDVALUE_AS_INT(object->std_arg);
        } else  {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg` to be an integer."
            );
        }

        task.type = kSpiTaskRdMaxSpeedHz;
        has_task = true;
    } else if STREQ("setWordSize", object->method) {
        if (STDVALUE_IS_SIZED_LIST(object->std_arg, 2) && STDVALUE_IS_INT(object->std_arg.list[0])
                                                      && STDVALUE_IS_INT(object->std_arg.list[1])) {
            fd = STDVALUE_AS_INT(object->std_arg.list[0]);
            task.bits = STDVALUE_AS_INT(object->std_arg.list[1]);
        } else {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg` to be a List<int> with size 2."
            );
        }

        task.type = kSpiTaskWrBitsPerWord;
        has_task = true;
    } else if STREQ("getWordSize", object->method) {
        if (STDVALUE_IS_INT(object->std_arg)) {
            fd = STDVALUE_AS_INT(object->std_arg);
        } else  {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg` to be an integer."
            );
        }

        task.type = kSpiTaskRdBitsPerWord;
        has_task = true;
    } else if STREQ("transmit", object->method) {
        if (object->std_arg.type == kStdMap) {
            temp = stdmap_get_str(&object->std_arg, "fd");
            if (temp && STDVALUE_IS_INT(*temp)) {
                fd = STDVALUE_AS_INT(*temp);
            } else {
                return platch_respond_illegal_arg_std(
                    responsehandle,
                    "Expected `arg['fd']` to be an integer."
                );
            }

            temp = stdmap_get_str(&object->std_arg, "speed");
            if (temp && STDVALUE_IS_INT(*temp)) {
                task.transfer.speed_hz = STDVALUE_AS_INT(*temp);
            } else {
                return platch_respond_illegal_arg_std(
                    responsehandle,
                    "Expected `arg['speed']` to be an integer."
                );
            }

            temp = stdmap_get_str(&object->std_arg, "delay");
            if (temp && STDVALUE_IS_INT(*temp)) {
                task.transfer.delay_usecs = STDVALUE_AS_INT(*temp);
            } else {
                return platch_respond_illegal_arg_std(
                    responsehandle,
                    "Expected `arg['delay']` to be an integer."
                );
            }

            temp = stdmap_get_str(&object->std_arg, "wordSize");
            if (temp && STDVALUE_IS_INT(*temp)) {
                task.transfer.bits_per_word = STDVALUE_AS_INT(*temp);
            } else {
                return platch_respond_illegal_arg_std(
                    responsehandle,
                    "Expected `arg['wordSize']` to be an integer."
                );
            }

            temp = stdmap_get_str(&object->std_arg, "csChange");
            if (temp && STDVALUE_IS_BOOL(*temp)) {
                task.transfer.cs_change = STDVALUE_AS_BOOL(*temp);
            } else {
                return platch_respond_illegal_arg_std(
                    responsehandle,
                    "Expected `arg['csChange']` to be an integer."
                );
            }

            temp = stdmap_get_str(&object->std_arg, "buffer");
            if (temp && temp->type == kStdUInt8Array) {
                task.transfer.len = temp->size;

                void *buf = malloc(temp->size);

                task.transfer.tx_buf = (__u64) ((uintptr_t) buf);
                task.transfer.rx_buf = task.transfer.tx_buf;

                memcpy(buf, temp->uint8array, temp->size);
            } else {
                return platch_respond_illegal_arg_std(
                    responsehandle,
                    "Expected `arg['buffer']` to be a uint8 array."
                );
            }
        }

        task.type = kSpiTaskTransmit;
        has_task = true;
    } else if STREQ("close", object->method) {
        if (STDVALUE_IS_INT(object->std_arg)) {
            fd = STDVALUE_AS_INT(object->std_arg);
        } else  {
            return platch_respond_illegal_arg_std(
                responsehandle,
                "Expected `arg` to be an integer."
            );
        }

        task.type = kSpiTaskClose;
        has_task = true;
    }

    if (has_task) {
        task.responsehandle = responsehandle;

        ok = spidevp_assign_task(fd, &task);
        if (ok == EBUSY) {
            return platch_respond_error_std(
                responsehandle,
                "busy",
                "a different task is running on the fd already",
                NULL
            );
        } else if (ok != 0) {
            return platch_respond_native_error_std(responsehandle, ok);
        }
    } else {
        return platch_respond_not_implemented(responsehandle);
    }

    return 0;
}

int spidevp_init(void) {
    spi_plugin.size_threads = 1;
    spi_plugin.threads = malloc(spi_plugin.size_threads * sizeof(struct spidevp_thread *));

    for (int i = 0; i < spi_plugin.size_threads; i++) {
        spi_plugin.threads[i] = malloc(sizeof(struct spidevp_thread));
        *(spi_plugin.threads[i]) = SPI_THREAD_INITIALIZER;
    }

    plugin_registry_set_receiver(SPI_PLUGIN_METHOD_CHANNEL, kStandardMethodCall, spidevp_on_receive);
    
    return 0;
}

int spidevp_deinit(void) {
    printf("[flutter_spidev] deinit.\n");
    return 0;
}