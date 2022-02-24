#define _GNU_SOURCE

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <math.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES2/gl2platform.h>
#include <systemd/sd-bus.h>
#include <inttypes.h>

#include <flutter-pi.h>
#include <platformchannel.h>
#include <pluginregistry.h>
#include <texture_registry.h>
#include <collection.h>
#include <compositor.h>

#include <plugins/omxplayer_video_player.h>

FILE_DESCR("omxplayer video_player plugin")

static struct {
    bool initialized;

    /// On creation of a new player,
    /// the id stored here will be used and incremented.
    int64_t next_unused_player_id;

    /// Collection of players.
    /*
    struct omxplayer_video_player **players;
    size_t size_players;
    size_t n_players;
    */

    struct concurrent_pointer_set players;

    /// The D-Bus that where omxplayer instances can be talked to.
    /// Typically the session dbus.
    //sd_bus *dbus;
    //pthread_t dbus_processor_thread;
} omxpvidpp = {
    .initialized = false,
    .next_unused_player_id = 1,
    .players = CPSET_INITIALIZER(CPSET_DEFAULT_MAX_SIZE)
};

/// Add a player instance to the player collection.
static int add_player(struct omxplayer_video_player *player) {
    return cpset_put(&omxpvidpp.players, player);
}

/// Get a player instance by its id.
struct omxplayer_video_player *get_player_by_id(int64_t player_id) {
    struct omxplayer_video_player *player;
    
    cpset_lock(&omxpvidpp.players);
    for_each_pointer_in_cpset(&omxpvidpp.players, player) {
        if (player->player_id == player_id) {
            cpset_unlock(&omxpvidpp.players);
            return player;
        }
    }

    cpset_unlock(&omxpvidpp.players);
    return NULL;
}

/// Get a player instance by its event channel name.
struct omxplayer_video_player *get_player_by_evch(const char *const event_channel_name) {
    struct omxplayer_video_player *player;
    
    cpset_lock(&omxpvidpp.players);
    for_each_pointer_in_cpset(&omxpvidpp.players, player) {
        if (strcmp(player->event_channel_name, event_channel_name) == 0) {
            cpset_unlock(&omxpvidpp.players);
            return player;
        }
    }

    cpset_unlock(&omxpvidpp.players);
    return NULL;
}

/// Remove a player instance from the player collection.
static int remove_player(struct omxplayer_video_player *player) {
    return cpset_remove(&omxpvidpp.players, player);
}

/// Get the player id from the given arg, which is a kStdMap.
/// (*player_id_out = arg['playerId'])
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int get_player_id_from_map_arg(
    struct std_value *arg,
    int64_t *player_id_out,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    int ok;

    if (arg->type != kStdMap) {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg` to be a Map"
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    struct std_value *id = stdmap_get_str(arg, "playerId");
    if (id == NULL || !STDVALUE_IS_INT(*id)) {
        ok = platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['playerId']` to be an integer"
        );
        if (ok != 0) return ok;

        return EINVAL;
    }

    *player_id_out = STDVALUE_AS_INT(*id);

    return 0;
}

/// Get the player associated with the id in the given arg, which is a kStdMap.
/// (*player_out = get_player_by_id(get_player_id_from_map_arg(arg)))
/// If an error ocurrs, this will respond with an illegal argument error to the given responsehandle.
static int get_player_from_map_arg(
    struct std_value *arg,
    struct omxplayer_video_player **player_out,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct omxplayer_video_player *player;
    int64_t player_id;
    int ok;

    player_id = 0;
    ok = get_player_id_from_map_arg(arg, &player_id, responsehandle);
    if (ok != 0) {
        return ok;
    }

    player = get_player_by_id(player_id);
    if (player == NULL) {
        ok = platch_respond_illegal_arg_std(responsehandle, "Expected `arg['playerId']` to be a valid player id.");
        if (ok != 0) return ok;

        return EINVAL;
    }

    *player_out = player;
    
    return 0;
}

static int get_orientation_from_rotation(double rotation) {
    if ((rotation <= 45) || (rotation > 315)) {
        rotation = 0;
    } else if (rotation <= 135) {
        rotation = 90;
    } else if (rotation <= 225) {
        rotation = 180;
    } else if (rotation <= 315) {
        rotation = 270;
    }

    return rotation;
}

/// Called on the flutter rasterizer thread when a players platform view is presented
/// for the first time after it was unmounted or initialized.
static int on_mount(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const struct platform_view_params *params,
    int zpos,
    void *userdata  
) {
    struct omxplayer_video_player *player = userdata;

    (void) view_id;
    (void) req;

    if (zpos == 1) {
        zpos = -126;
    }

    struct aa_rect rect = get_aa_bounding_rect(params->rect);

    return cqueue_enqueue(
        &player->mgr->task_queue,
        &(struct omxplayer_mgr_task) {
            .type = kUpdateView,
            .responsehandle = NULL,
            .offset_x = round(rect.offset.x),
            .offset_y = round(rect.offset.y),
            .width = round(rect.size.x),
            .height = round(rect.size.y),
            .zpos = zpos,
            .orientation = get_orientation_from_rotation(params->rotation)
        }
    );
}

/// Called on the flutter rasterizer thread when a players platform view is not present
/// in the currently being drawn frame after it was present in the previous frame.
static int on_unmount(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    void *userdata
) {
    struct omxplayer_video_player *player = userdata;

    (void) view_id;
    (void) req;

    return cqueue_enqueue(
        &player->mgr->task_queue,
        &(struct omxplayer_mgr_task) {
            .type = kUpdateView,
            .offset_x = 0,
            .offset_y = 0,
            .width = 1,
            .height = 1,
            .zpos = -128
        }
    );
}

/// Called on the flutter rasterizer thread when the presentation details (offset, mutations, dimensions, zpos)
/// changed from the previous frame.
static int on_update_view(
    int64_t view_id,
    struct drmdev_atomic_req *req,
    const struct platform_view_params *params,
    int zpos,
    void *userdata
) {
    struct omxplayer_video_player *player = userdata;

    (void) view_id;
    (void) req;

    if (zpos == 1) {
        zpos = -126;
    }

    struct aa_rect rect = get_aa_bounding_rect(params->rect);

    return cqueue_enqueue(
        &player->mgr->task_queue,
        &(struct omxplayer_mgr_task) {
            .type = kUpdateView,
            .responsehandle = NULL,
            .offset_x = rect.offset.x,
            .offset_y = rect.offset.y,
            .width = rect.size.x,
            .height = rect.size.y,
            .zpos = zpos,
            .orientation = get_orientation_from_rotation(params->rotation)
        }
    );
}

static int respond_sd_bus_error(
    FlutterPlatformMessageResponseHandle *handle,
    sd_bus_error *err
) {
    char str[256];

    snprintf(str, sizeof(str), "%s: %s", err->name, err->message);

    return platch_respond_error_std(
        handle,
        "dbus-error",
        str,
        NULL
    );
}

/// Unfortunately, we can't use sd_bus for this, because it
/// wraps some things in containers.
static int get_dbus_property(
    sd_bus *bus,
    const char *destination,
    const char *path,
    const char *interface,
    const char *member,
    sd_bus_error *ret_error,
    char type,
    void *ret_ptr
) {
    sd_bus_message *msg;
    int ok;

    ok = sd_bus_call_method(
        bus,
        destination,
        path,
        DBUS_PROPERTY_FACE,
        DBUS_PROPERTY_GET,
        ret_error,
        &msg,
        "ss",
        interface,
        member
    );
    if (ok < 0) {
        fprintf(stderr, "[omxplayer_video_player plugin] Could not read DBus property: %s, %s\n", ret_error->name, ret_error->message);
        return -ok;
    }

    ok = sd_bus_message_read_basic(msg, type, ret_ptr);
    if (ok < 0) {
        fprintf(stderr, "[omxplayer_video_player plugin] Could not read DBus property: %s\n", strerror(-ok));
        sd_bus_message_unref(msg);
        return -ok;
    }

    sd_bus_message_unref(msg);

    return 0;
}

/// Callback to be called when the omxplayer manager receives
/// a DBus message. (Currently only used for listening to NameOwnerChanged messages,
/// to find out when omxplayer registers to the dbus.)
static int mgr_on_dbus_message(
    sd_bus_message *m,
    void *userdata,
    sd_bus_error *ret_error
) {
    struct omxplayer_mgr_task *task;
    const char *sender, *member;
    char *old_owner, *new_owner, *name;
    int ok;

    (void) ret_error;

    task = userdata;

    sender = sd_bus_message_get_sender(m);
    member = sd_bus_message_get_member(m);

    if (STREQ(sender, "org.freedesktop.DBus") && STREQ(member, "NameOwnerChanged")) {
        ok = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
        if (ok < 0) {
            fprintf(stderr, "Could not read message");
            return -1;
        }

        if STREQ(name, task->omxplayer_dbus_name) {
            task->omxplayer_online = true;
        }
    }

    return 0;
}

/// The entry function of the manager thread.
/// Manager thread has the ownership over the player / manager / task queue objects
/// and must free them when it quits.
static void *mgr_entry(void *userdata) {
    struct omxplayer_mgr_task task;
    struct concurrent_queue *q;
    struct omxplayer_mgr *mgr;
    sd_bus_message *msg;
    sd_bus_error err;
    sd_bus_slot *slot;
    int64_t duration_us, video_width, video_height, current_zpos, current_orientation;
    sd_bus *bus;
    pid_t omxplayer_pid;
    char dbus_name[256];
    bool has_sent_initialized_event;
    bool is_stream;
    int ok;

    (void) current_orientation;

    mgr = userdata;
    q = &mgr->task_queue;

    // dequeue the first task of the queue (creation task)
    ok = cqueue_dequeue(q, &task);
    if (ok != 0) {
        fprintf(stderr, "[omxplayer_video_player plugin] Could not dequeue creation task in manager thread. cqueue_dequeue: %s\n", strerror(ok));
        platch_respond_error_std(
            task.responsehandle,
            "internal-error",
            "Could not dequeue creation task in manager thread.",
            NULL
        );
        goto fail_remove_evch_listener;
    }

    // check that it really is a creation task
    if (task.type != kCreate || task.responsehandle == NULL) {
        fprintf(stderr, "[omxplayer_video_player plugin] First task of manager thread is not a creation task.\n");
        platch_respond_error_std(
            task.responsehandle,
            "internal-error",
            "First task of manager thread is not a creation task.",
            NULL
        );
        goto fail_remove_evch_listener;
    }

    // determine whether we're watching a stream or not.
    // this is a heuristic. unfortunately, omxplayer itself doesn't even know whether it's playing
    // back a stream or a video file.
    if (strstr(mgr->player->video_uri, "rtsp://") == mgr->player->video_uri) {
        is_stream = true;
    } else {
        is_stream = false;
    }

    // generate the player name
    snprintf(
        dbus_name,
        sizeof(dbus_name),
        "org.mpris.MediaPlayer2.omxplayer_%d_%" PRId64,
        (int) getpid(),
        mgr->player->player_id
    );

    // open the session dbus
    ok = sd_bus_open_user(&bus);
    if (ok < 0) {
        fprintf(stderr, "[omxplayer_video_player plugin] Could not open DBus in manager thread. sd_bus_open_user: %s\n", strerror(-ok));
        platch_respond_native_error_std(task.responsehandle, -ok);
        goto fail_remove_evch_listener;
    }

    // register a callbacks that tells us when
    // omxplayer has registered to the dbus
    task.omxplayer_online = false;
    task.omxplayer_dbus_name = dbus_name;
    ok = sd_bus_match_signal(
        bus,
        &slot,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "NameOwnerChanged",
        mgr_on_dbus_message,
        &task
    );
    if (ok < 0) {
        fprintf(stderr, "[omxplayer_video_player plugin] Could not wait for omxplayer DBus registration in manager thread. sd_bus_match_signal: %s\n", strerror(-ok));
        platch_respond_native_error_std(task.responsehandle, -ok);
        goto fail_close_dbus;
    }

    // spawn the omxplayer process
    current_zpos = -128;
    current_orientation = task.orientation;
    pid_t me = fork();
    if (me == 0) {
        char orientation_str[16] = {0};
        snprintf(orientation_str, sizeof orientation_str, "%d", task.orientation);

        // I'm the child!
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        int _ok = execvp(
            "omxplayer.bin",
            (char*[]) {
                "omxplayer.bin",
                "--nohdmiclocksync",
                "--no-osd",
                "--no-keys",
                "--loop",
                "--layer", "-128",
                "--win", "0,0,1,1",
                "--orientation", orientation_str, 
                "--dbus_name", dbus_name,
                mgr->player->video_uri,
                NULL
            }
        );

        if (_ok != 0) {
            exit(_ok);
        }
        exit(0);
    } else if (me > 0) {
        // I'm the parent!
        omxplayer_pid = me;
    } else if (me < 0) {
        // something went wrong.
        ok = errno;
        perror("[omxplayer_video_player plugin] Could not spawn omxplayer subprocess. fork");
        platch_respond_native_error_std(task.responsehandle, ok);
        goto fail_unref_slot;
    }

    while (!task.omxplayer_online) {
        ok = sd_bus_wait(bus, 1000*1000*5);
        if (ok < 0) {
            ok = -ok;
            fprintf(stderr, "[omxplayer_video_player plugin] Could not wait for sd bus messages on manager thread: %s\n", strerror(ok));
            platch_respond_native_error_std(task.responsehandle, ok);
            goto fail_kill_unregistered_player;
        }

        ok = sd_bus_process(bus, NULL);
        if (ok < 0) {
            ok = -ok;
            fprintf(stderr, "[omxplayer_video_player plugin] Could not wait for sd bus messages on manager thread: %s\n", strerror(ok));
            platch_respond_native_error_std(task.responsehandle, ok);
            goto fail_kill_unregistered_player;
        }
    }

    sd_bus_slot_unref(slot);
    slot = NULL;

    duration_us = 0;
    ok = get_dbus_property(
        bus,
        dbus_name,
        DBUS_OMXPLAYER_OBJECT,
        DBUS_OMXPLAYER_PLAYER_FACE,
        "Duration",
        &err,
        'x',
        &duration_us
    );
    if (ok != 0) {
        respond_sd_bus_error(task.responsehandle, &err);
        goto fail_kill_registered_player;
    }

    // wait for the first frame to appear
    {
        struct timespec delta = {
            .tv_sec = 0,
            .tv_nsec = 300*1000*1000
        };
        while (nanosleep(&delta, &delta));
    }

    // pause right on the first frame
    ok = sd_bus_call_method(
        bus,
        dbus_name,
        DBUS_OMXPLAYER_OBJECT,
        DBUS_OMXPLAYER_PLAYER_FACE,
        "Play",
        &err,
        &msg,
        ""
    );
    if (ok < 0) {
        fprintf(stderr, "[omxplayer_video_player plugin] Could not send initial pause message: %s, %s\n", err.name, err.message);
        respond_sd_bus_error(task.responsehandle, &err);
        goto fail_kill_registered_player;
    }

    sd_bus_message_unref(msg);
    msg = NULL;

    // get the video duration
    duration_us = 0;
    ok = get_dbus_property(
        bus,
        dbus_name,
        DBUS_OMXPLAYER_OBJECT,
        DBUS_OMXPLAYER_PLAYER_FACE,
        "Duration",
        &err,
        'x',
        &duration_us
    );
    if (ok != 0) {
        respond_sd_bus_error(task.responsehandle, &err);
        goto fail_kill_registered_player;
    }

    // get the video width
    video_width = 0;
    ok = get_dbus_property(
        bus,
        dbus_name,
        DBUS_OMXPLAYER_OBJECT,
        DBUS_OMXPLAYER_PLAYER_FACE,
        "ResWidth",
        &err,
        'x',
        &video_width
    );
    if (ok < 0) {
        respond_sd_bus_error(task.responsehandle, &err);
        goto fail_kill_registered_player;
    }

    // get the video width
    video_height = 0;
    ok = get_dbus_property(
        bus,
        dbus_name,
        DBUS_OMXPLAYER_OBJECT,
        DBUS_OMXPLAYER_PLAYER_FACE,
        "ResHeight",
        &err,
        'x',
        &video_height
    );
    if (ok < 0) {
        respond_sd_bus_error(task.responsehandle, &err);
        goto fail_kill_registered_player;
    }


    LOG_DEBUG("respond success on_create(). player_id: %" PRId64 "\n", mgr->player->player_id);
    // creation was a success! respond to the dart-side with our player id.
    platch_respond_success_std(task.responsehandle, &STDINT64(mgr->player->player_id));

    has_sent_initialized_event = false;
    while (1) {
        ok = cqueue_dequeue(q, &task);
        
        if (task.type == kUpdateView) {
            struct omxplayer_mgr_task *peek;

            cqueue_lock(q);

            cqueue_peek_locked(q, (void**) &peek);
            while ((peek != NULL) && (peek->type == kUpdateView)) {
                cqueue_dequeue_locked(q, &task);
                cqueue_peek_locked(q, (void**) &peek);
            }

            cqueue_unlock(q);
        }
        
        if (task.type == kCreate) {
            printf("[omxplayer_video_player plugin] Omxplayer manager got a creation task, even though the player is already running.\n");
        } else if (task.type == kDispose) {
            if (mgr->player->has_view) {
                fprintf(stderr, "[omxplayer_video_player plugin] flutter attempted to dispose the video player before its view was disposed.\n");

                compositor_remove_view_callbacks(mgr->player->view_id);

                mgr->player->has_view = false;
                mgr->player->view_id = -1;
            }

            // tell omxplayer to quit
            ok = sd_bus_call_method(
                bus,
                dbus_name,
                DBUS_OMXPLAYER_OBJECT,
                DBUS_OMXPLAYER_ROOT_FACE,
                "Quit",
                &err,
                NULL,
                ""
            );
            if (ok < 0) {
                fprintf(stderr, "[omxplayer_video_player plugin] Could not send Quit message to omxplayer: %s, %s\n", err.name, err.message);
                respond_sd_bus_error(task.responsehandle, &err);
                continue;
            }

            ok = (int) waitpid(omxplayer_pid, NULL, 0);
            if (ok < 0) {
                fprintf(stderr, "[omxplayer_video_player plugin] omxplayer quit with exit code %d\n", ok);
            }

            sd_bus_unref(bus);
            
            plugin_registry_remove_receiver(mgr->player->event_channel_name);
            
            remove_player(mgr->player);
            
            free(mgr->player);
            mgr->player = NULL;

            cqueue_deinit(&mgr->task_queue);
            
            free(mgr);
            mgr = NULL;

            platch_respond_success_std(task.responsehandle, NULL);

            break;
        } else if (task.type == kListen) {
            platch_respond_success_std(task.responsehandle, NULL);

            if (!has_sent_initialized_event) {
                platch_send_success_event_std(
                    mgr->player->event_channel_name,
                    &(struct std_value) {
                        .type = kStdMap,
                        .size = 4,
                        .keys = (struct std_value[4]) {
                            STDSTRING("event"),
                            STDSTRING("duration"),
                            STDSTRING("width"),
                            STDSTRING("height")
                        },
                        .values = (struct std_value[4]) {
                            STDSTRING("initialized"),
                            STDINT64(is_stream? INT64_MAX : duration_us / 1000),
                            STDINT32(video_width),
                            STDINT32(video_height)
                        }
                    }
                );

                has_sent_initialized_event = true;
            }
        } else if (task.type == kUnlisten) {
            platch_respond_success_std(task.responsehandle, NULL);
        } else if (task.type == kPlay) {
            ok = sd_bus_call_method(
                bus,
                dbus_name,
                DBUS_OMXPLAYER_OBJECT,
                DBUS_OMXPLAYER_PLAYER_FACE,
                "Play",
                &err,
                NULL,
                ""
            );
            if (ok < 0) {
                fprintf(stderr, "[omxplayer_video_player plugin] Could not send play message: %s, %s\n", err.name, err.message);
                respond_sd_bus_error(task.responsehandle, &err);
                continue;
            }

            platch_respond_success_std(task.responsehandle, NULL);
        } else if (task.type == kPause) {
            ok = sd_bus_call_method(
                bus,
                dbus_name,
                DBUS_OMXPLAYER_OBJECT,
                DBUS_OMXPLAYER_PLAYER_FACE,
                "Pause",
                &err,
                NULL,
                ""
            );
            if (ok < 0) {
                fprintf(stderr, "[omxplayer_video_player plugin] Could not send pause message: %s, %s\n", err.name, err.message);
                respond_sd_bus_error(task.responsehandle, &err);
                continue;
            }

            msg = NULL;

            platch_respond_success_std(task.responsehandle, NULL);   
        } else if (task.type == kUpdateView) {
            char video_pos_str[256];

            // Use integers here even if omxplayer supports floats because if we print floats,
            // snprintf might use `,` as the decimal delimiter depending on the locale.
            // This is only an issue because I set the application to be locale-aware using setlocale(LC_ALL, "")
            // since that's needed for locale support.
            snprintf(
                video_pos_str,
                sizeof(video_pos_str),
                "%d %d %d %d",
                task.offset_x,
                task.offset_y,
                task.offset_x + task.width,
                task.offset_y + task.height
            );

            ok = sd_bus_call_method(
                bus,
                dbus_name,
                DBUS_OMXPLAYER_OBJECT,
                DBUS_OMXPLAYER_PLAYER_FACE,
                "VideoPos",
                &err,
                NULL,
                "os",
                "/obj/not/used",
                video_pos_str
            );
            if (ok < 0) {
                fprintf(stderr, "[omxplayer_video_player plugin] Could not update omxplayer viewport. %s, %s\n", err.name, err.message);
                continue;
            }

            if (current_zpos != task.zpos) {
                ok = sd_bus_call_method(
                    bus,
                    dbus_name,
                    DBUS_OMXPLAYER_OBJECT,
                    DBUS_OMXPLAYER_PLAYER_FACE,
                    "SetLayer",
                    &err,
                    NULL,
                    "x",
                    (int64_t) task.zpos
                );
                if (ok < 0) {
                    fprintf(stderr, "[omxplayer_video_player plugin] Could not update omxplayer layer. %s, %s\n", err.name, err.message);
                    continue;
                }

                current_zpos = task.zpos;
            }

#ifdef OMXPLAYER_SUPPORTS_RUNTIME_ROTATION
            if (current_orientation != task.orientation) {
                ok = sd_bus_call_method(
                    bus,
                    dbus_name,
                    DBUS_OMXPLAYER_OBJECT,
                    DBUS_OMXPLAYER_PLAYER_FACE,
                    "SetTransform",
                    &err,
                    NULL,
                    "x",
                    (360 - (int64_t) task.orientation) % 360
                );
                if (ok < 0) {
                    fprintf(stderr, "[omxplayer_video_player plugin] Could not update omxplayer rotation. %s, %s\n", err.name, err.message);
                    continue;
                }
                current_orientation = task.orientation;
            }
#endif
        } else if (task.type == kGetPosition) {
            int64_t position = 0;
            
            ok = get_dbus_property(
                bus,
                dbus_name,
                DBUS_OMXPLAYER_OBJECT,
                DBUS_OMXPLAYER_PLAYER_FACE,
                "Position",
                &err,
                'x',
                &position
            );
            if (ok != 0) {
                fprintf(stderr, "[omxplayer_video_player plugin] Could not get omxplayer position: %s, %s\n", err.name, err.message);
                respond_sd_bus_error(task.responsehandle, &err);
                continue;
            }
            
            position = position / 1000;

            platch_respond_success_std(task.responsehandle, &STDINT64(position));
        } else if (task.type == kSetPosition) {
            if (is_stream) {
                if (task.position == -1) {
                    // TODO: implement seek-to-end

                    platch_respond_success_std(
                        task.responsehandle,
                        NULL
                    );
                } else {
                    // Don't allow flutter to seek to anything other than the end on a stream.
                    fprintf(stderr, "[omxplayer_video_player plugin] Flutter attempted to seek on non-seekable video (a stream).\n");

                    platch_respond_error_std(
                        task.responsehandle,
                        "state-error",
                        "Attempted to seek on non-seekable video (a stream)",
                        NULL
                    );
                }
            } else {
                ok = sd_bus_call_method(
                    bus,
                    dbus_name,
                    DBUS_OMXPLAYER_OBJECT,
                    DBUS_OMXPLAYER_PLAYER_FACE,
                    "SetPosition",
                    &err,
                    NULL,
                    "ox",
                    "/path/not/used",
                    (int64_t) (task.position * 1000)
                );
                if (ok < 0) {
                    fprintf(stderr, "[omxplayer_video_player plugin] Could not set omxplayer position: %s, %s, %s\n", strerror(-ok), err.name, err.message);
                    respond_sd_bus_error(task.responsehandle, &err);
                    continue;
                }

                platch_respond_success_std(task.responsehandle, NULL);
            }
        } else if (task.type == kSetLooping) {
            platch_respond_success_std(task.responsehandle, NULL);
        } else if (task.type == kSetVolume) {
            ok = sd_bus_call_method(
                bus,
                dbus_name,
                DBUS_OMXPLAYER_OBJECT,
                DBUS_PROPERTY_FACE,
                DBUS_PROPRETY_SET,
                &err,
                NULL,
                "ssd",
                DBUS_OMXPLAYER_PLAYER_FACE,
                "Volume",
                (double) task.volume
            );
            if (ok < 0) {
                fprintf(stderr, "[omxplayer_video_player plugin] Could not set omxplayer volume: %s, %s\n", err.name, err.message);
                respond_sd_bus_error(task.responsehandle, &err);
                continue;
            }

            platch_respond_success_std(task.responsehandle, NULL);
        }
    }
    
    return (void*) EXIT_SUCCESS;


    fail_kill_registered_player:
    kill(omxplayer_pid, SIGKILL);
    waitpid(omxplayer_pid, NULL, 0);
    goto fail_close_dbus;

    fail_kill_unregistered_player:
    kill(omxplayer_pid, SIGKILL);
    waitpid(omxplayer_pid, NULL, 0);

    fail_unref_slot:
    sd_bus_slot_unref(slot);
    slot = NULL;

    fail_close_dbus:
    sd_bus_unref(bus);

    fail_remove_evch_listener:
    plugin_registry_remove_receiver(mgr->player->event_channel_name);
    remove_player(mgr->player);
    free(mgr->player);
    cqueue_deinit(&mgr->task_queue);
    free(mgr);
    mgr = NULL;
    return (void*) EXIT_FAILURE;
}

/// Ensures the bindings to libsystemd are initialized.
static int ensure_binding_initialized(void) {
    int ok;

    if (omxpvidpp.initialized) return 0;

    ok = access("/usr/bin/omxplayer.bin", X_OK);
    if (ok < 0) {
        fprintf(stderr, "[omxplayer_video_player plugin] omxplayer doesn't seem to be installed. Please install using 'sudo apt install omxplayer'. access: %s\n", strerror(errno));
        return errno;
    }

    omxpvidpp.initialized = true;

    return 0;
}

/// Respond to the handle with a "initialization failed" message.
static int respond_init_failed(FlutterPlatformMessageResponseHandle *handle) {
    return platch_respond_error_std(
        handle,
        "couldnotinit",
        "omxplayer_video_player plugin failed to initialize libsystemd bindings. See flutter-pi log for details.",
        NULL
    );
}

/*******************************************************
 * CHANNEL HANDLERS                                    *
 * handle method calls on the method and event channel *
 *******************************************************/
static int on_receive_evch(
    char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    struct omxplayer_video_player *player;

    player = get_player_by_evch(channel);
    if (player == NULL) {
        return platch_respond_not_implemented(responsehandle);
    }

    if STREQ("listen", object->method) {
        return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
            .type = kListen,
            .responsehandle = responsehandle
        });
    } else if STREQ("cancel", object->method) {
        return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
            .type = kUnlisten,
            .responsehandle = responsehandle
        });
    } else {
        return platch_respond_not_implemented(responsehandle);
    }
}

static int on_initialize(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    int ok;

    (void) arg;

    ok = ensure_binding_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    LOG_DEBUG("on_initialize\n");

    return platch_respond_success_std(responsehandle, NULL);
}

/// Creates a new video player.
/// Should respond to the platform message when the player has established its viewport.
static int on_create(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    struct omxplayer_mgr *mgr;
    enum   data_source_type source_type;
    struct std_value *temp;
    char *asset, *uri, *package_name, *format_hint;
    int ok;

    (void) source_type;
    (void) package_name;
    (void) format_hint;

    ok = ensure_binding_initialized();
    if (ok != 0) {
        return respond_init_failed(responsehandle);
    }

    temp = stdmap_get_str(arg, "sourceType");
    if (temp != NULL && STDVALUE_IS_STRING(*temp)) {
        char *source_type_str = temp->string_value;

        if STREQ("DataSourceType.asset", source_type_str) {
            source_type = kDataSourceTypeAsset;
        } else if STREQ("DataSourceType.network", source_type_str) {
            source_type = kDataSourceTypeNetwork;
        } else if STREQ("DataSourceType.file", source_type_str) {
            source_type = kDataSourceTypeFile;
        } else {
            goto invalid_source_type;
        }
    } else {
        invalid_source_type:

        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['sourceType']` to be a stringification of the [DataSourceType] enum."
        );
    }

    temp = stdmap_get_str(arg, "asset");
    if (temp == NULL || temp->type == kStdNull) {
        asset = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        asset = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['asset']` to be a String or null."
        );
    }

    temp = stdmap_get_str(arg, "uri");
    if (temp == NULL || temp->type == kStdNull) {
        uri = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        uri = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['uri']` to be a String or null."
        );
    }

    temp = stdmap_get_str(arg, "packageName");
    if (temp == NULL || temp->type == kStdNull) {
        package_name = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        package_name = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['packageName']` to be a String or null."
        );
    }

    temp = stdmap_get_str(arg, "formatHint");
    if (temp == NULL || temp->type == kStdNull) {
        format_hint = NULL;
    } else if (temp != NULL && temp->type == kStdString) {
        format_hint = temp->string_value;
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['formatHint']` to be a String or null."
        );
    }

    LOG_DEBUG(
        "on_create(sourceType: %s, asset: %s, uri: \"%s\", packageName: %s, formatHint: %s)\n",
        source_type == kDataSourceTypeAsset ? "asset" : source_type == kDataSourceTypeNetwork ? "network" : "file",
        asset,
        uri,
        package_name,
        format_hint
    );

    mgr = calloc(1, sizeof *mgr);
    if (mgr == NULL) {
        return platch_respond_native_error_std(responsehandle, ENOMEM);
    }

    ok = cqueue_init(&mgr->task_queue, sizeof(struct omxplayer_mgr_task), CQUEUE_DEFAULT_MAX_SIZE);
    if (ok != 0) {
        goto fail_free_mgr;
    }

    // Allocate the player metadata
    player = calloc(1, sizeof(*player));
    if (player == NULL) {
        goto fail_deinit_task_queue;
    }

    player->player_id = omxpvidpp.next_unused_player_id++;
    player->mgr = mgr;
    if (asset != NULL) {
        snprintf(player->video_uri, sizeof(player->video_uri), "%s/%s", flutterpi.flutter.asset_bundle_path, asset);
    } else {
        strncpy(player->video_uri, uri, sizeof(player->video_uri));
    }
    
    mgr->player = player;

    ok = cqueue_enqueue(&mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kCreate,
        .responsehandle = responsehandle,
        .orientation = flutterpi.view.rotation
    });

    if (ok != 0) {
        goto fail_free_player;
    }

    snprintf(
        player->event_channel_name,
        sizeof(player->event_channel_name),
        "flutter.io/omxplayerVideoPlayer/videoEvents%" PRId64,
        player->player_id
    );

    // add it to our player collection
    ok = add_player(player);
    if (ok != 0) {
        goto fail_free_player;
    }

    // set a receiver on the videoEvents event channel
    ok = plugin_registry_set_receiver(
        player->event_channel_name,
        kStandardMethodCall,
        on_receive_evch
    );
    if (ok != 0) {
        goto fail_remove_player;
    }

    ok = pthread_create(&mgr->thread, NULL, mgr_entry, mgr);
    if (ok != 0) {
        goto fail_remove_evch_listener;
    }

    return 0;


    fail_remove_evch_listener:
    plugin_registry_remove_receiver(player->event_channel_name);

    fail_remove_player:
    remove_player(player);

    fail_free_player:
    free(player);
    player = NULL;

    fail_deinit_task_queue:
    cqueue_deinit(&mgr->task_queue);

    fail_free_mgr:
    free(mgr);
    mgr = NULL;

    return platch_respond_native_error_std(responsehandle, ok);
}

static int on_dispose(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) {
        return ok;
    }

    LOG_DEBUG("on_dispose(%"PRId64")\n", player->player_id);

    return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kDispose,
        .responsehandle = responsehandle
    });
}

static int on_set_looping(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    struct std_value *temp;
    bool loop;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "looping");
    if (STDVALUE_IS_BOOL(*temp)) {
        loop = STDVALUE_AS_BOOL(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['looping']` to be a boolean."
        );
    }

    LOG_DEBUG("on_set_looping(player_id: %"PRId64", looping: %s)\n", player->player_id, loop ? "yes" : "no");

    return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kSetLooping,
        .loop = loop,
        .responsehandle = responsehandle
    });
}

static int on_set_volume(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    struct std_value *temp;
    float volume;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "volume");
    if (STDVALUE_IS_FLOAT(*temp)) {
        volume = STDVALUE_AS_FLOAT(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['volume']` to be a float/double."
        );
    }

    LOG_DEBUG("on_set_volume(player_id: %"PRId64", volume: %f)\n", player->player_id, volume);

    return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kSetVolume,
        .volume = volume,
        .responsehandle = responsehandle
    });
}

static int on_play(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    LOG_DEBUG("on_play(player_id: %"PRId64")\n", player->player_id);

    return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kPlay,
        .responsehandle = responsehandle
    });
}

static int on_get_position(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    LOG_DEBUG("on_get_position(player_id: %"PRId64")\n", player->player_id);

    return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kGetPosition,
        .responsehandle = responsehandle
    });
}

static int on_seek_to(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    struct std_value *temp;
    int64_t position;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "position");
    if (STDVALUE_IS_INT(*temp)) {
        position = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['position']` to be an integer."
        );
    }

    LOG_DEBUG("on_seek_to(player_id: %"PRId64", position: %"PRId64")\n", player->player_id, position);

    return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kSetPosition,
        .position = position,
        .responsehandle = responsehandle
    });
}

static int on_pause(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    LOG_DEBUG("on_pause(player_id: %"PRId64")\n", player->player_id);

    return cqueue_enqueue(&player->mgr->task_queue, &(const struct omxplayer_mgr_task) {
        .type = kPause,
        .responsehandle = responsehandle
    });
}

static int on_create_platform_view(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    struct std_value *temp;
    int64_t view_id;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "platformViewId");
    if (STDVALUE_IS_INT(*temp)) {
        view_id = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['platformViewId']` to be an integer."
        );
    }

    LOG_DEBUG("on_create_platform_view(player_id: %"PRId64", platform view id: %"PRId64")\n", player->player_id, view_id);
    
    if (player->has_view) {
        LOG_ERROR("Flutter attempted to register more than one platform view for one player instance.\n");
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Attempted to register more than one platform view for this player instance."
        );
    } else {
        ok = compositor_set_view_callbacks(
            view_id,
            on_mount,
            on_unmount,
            on_update_view,
            NULL,
            player
        );
        if (ok != 0) {
            return platch_respond_native_error_std(responsehandle, ok);
        }
        
        player->has_view = true;
        player->view_id = view_id;

        return platch_respond_success_std(responsehandle, NULL);
    }
}

static int on_dispose_platform_view(
    struct std_value *arg,
    FlutterPlatformMessageResponseHandle* responsehandle
) {
    struct omxplayer_video_player *player;
    struct std_value *temp;
    int64_t view_id;
    int ok;

    ok = get_player_from_map_arg(arg, &player, responsehandle);
    if (ok != 0) return ok;

    temp = stdmap_get_str(arg, "platformViewId");
    if (STDVALUE_IS_INT(*temp)) {
        view_id = STDVALUE_AS_INT(*temp);
    } else {
        return platch_respond_illegal_arg_std(
            responsehandle,
            "Expected `arg['platformViewId']` to be an integer."
        );
    }

    LOG_DEBUG("on_dispose_platform_view(player_id: %"PRId64", platform view id: %"PRId64")\n", player->player_id, view_id);

    if (player->view_id != view_id) {
        LOG_ERROR("Flutter attempted to dispose an omxplayer platform view that is not associated with this player.\n");

        return platch_respond_illegal_arg_std(responsehandle, "Attempted to dispose on omxplayer view that is not associated with `arg['playerId']`.");
    } else {
        ok = compositor_remove_view_callbacks(view_id);
        if (ok != 0) {
            LOG_ERROR("Could not remove view callbacks for platform view %" PRId64 ". compositor_remove_view_callbacks: %s\n", view_id, strerror(ok));
            return platch_respond_native_error_std(responsehandle, ok);
        }

        player->has_view = false;
        player->view_id = -1;

        // hide omxplayer
        cqueue_enqueue(&player->mgr->task_queue, &(struct omxplayer_mgr_task) {
            .type = kUpdateView,
            .offset_x = 0,
            .offset_y = 0,
            .width = 1,
            .height = 1,
            .zpos = -128
        });

        return platch_respond_success_std(responsehandle, NULL);
    }
}

/// Called when a platform channel object is received on the method channel.
/// Finds out which method was called an then calls the corresponding above method,
/// or else responds with not implemented.
static int on_receive_mch(
    char *channel,
    struct platch_obj *object,
    FlutterPlatformMessageResponseHandle *responsehandle
) {
    (void) channel;

    if STREQ("init", object->method) {
        return on_initialize(&object->std_arg, responsehandle);
    } else if STREQ("create", object->method) {
        return on_create(&object->std_arg, responsehandle);
    } else if STREQ("dispose", object->method) {
        return on_dispose(&object->std_arg, responsehandle);
    } else if STREQ("setLooping", object->method) {
        return on_set_looping(&object->std_arg, responsehandle);
    } else if STREQ("setVolume", object->method) {
        return on_set_volume(&object->std_arg, responsehandle);
    } else if STREQ("play", object->method) {
        return on_play(&object->std_arg, responsehandle);
    } else if STREQ("pause", object->method) {
        return on_pause(&object->std_arg, responsehandle);
    } else if STREQ("getPosition", object->method) {
        return on_get_position(&object->std_arg, responsehandle);
    } else if STREQ("seekTo", object->method) {
        return on_seek_to(&object->std_arg, responsehandle);
    } else if STREQ("createPlatformView", object->method) {
        return on_create_platform_view(&object->std_arg, responsehandle);
    } else if STREQ("disposePlatformView", object->method) {
        return on_dispose_platform_view(&object->std_arg, responsehandle);
    }
    
    return platch_respond_not_implemented(responsehandle);
}

int8_t omxpvidpp_is_present(void) {
    return plugin_registry_is_plugin_present(flutterpi.plugin_registry, "omxplayer video_player plugin");
}

enum plugin_init_result omxpvidpp_init(struct flutterpi *flutterpi, void **userdata_out) {
    int ok;

    (void) flutterpi;
    (void) userdata_out;

    ok = plugin_registry_set_receiver("flutter.io/omxplayerVideoPlayer", kStandardMethodCall, on_receive_mch);
    if (ok != 0) {
        return kError_PluginInitResult;
    }

    return kInitialized_PluginInitResult;
}

void omxpvidpp_deinit(struct flutterpi *flutterpi, void *userdata) {
    plugin_registry_remove_receiver("flutter.io/omxplayerVideoPlayer");
    (void) flutterpi;
    (void) userdata;
}

FLUTTERPI_PLUGIN(
    "omxplayer video_player plugin",
    omxpvidpp,
    omxpvidpp_init,
    omxpvidpp_deinit
)