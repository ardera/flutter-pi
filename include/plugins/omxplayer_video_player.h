#ifndef _OMXPLAYER_VIDEO_PLAYER_PLUGIN_H
#define _OMXPLAYER_VIDEO_PLAYER_PLUGIN_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

#include <systemd/sd-bus.h>
#include <flutter_embedder.h>

#include <collection.h>

#define DBUS_OMXPLAYER_OBJECT "/org/mpris/MediaPlayer2"
#define DBUS_OMXPLAYER_PLAYER_FACE "org.mpris.MediaPlayer2.Player"
#define DBUS_OMXPLAYER_ROOT_FACE "org.mpris.MediaPlayer2"
#define DBUS_PROPERTY_FACE "org.freedesktop.DBus.Properties"
#define DBUS_PROPERTY_GET "Get"
#define DBUS_PROPRETY_SET "Set"

struct omxplayer_mgr;

struct omxplayer_video_player {
	int64_t player_id;
	char 	event_channel_name[256];
	char    video_uri[256];

	bool    has_view;
	int64_t view_id;

	struct omxplayer_mgr *mgr;
};

struct omxplayer_mgr {
	pthread_t thread;
	struct omxplayer_video_player *player;
	struct concurrent_queue task_queue;
};

enum omxplayer_mgr_task_type {
	kCreate,
	kDispose,
	kListen,
	kUnlisten,
	kSetLooping,
	kSetVolume,
	kPlay,
	kPause,
	kGetPosition,
	kSetPosition,
	kUpdateView
};

struct omxplayer_mgr_task {
	enum omxplayer_mgr_task_type type;

	FlutterPlatformMessageResponseHandle *responsehandle;
	
	union {
		struct {
			int orientation;
			union {
				struct {
					char *omxplayer_dbus_name;
					bool omxplayer_online;
				};
				struct {
					bool visible;
					int offset_x, offset_y;
					int width, height;
					int zpos;
				};
			};
		};
		bool loop;
		float volume;
		int64_t position;
	};
};

enum data_source_type {
	kDataSourceTypeAsset,
	kDataSourceTypeNetwork,
	kDataSourceTypeFile
};

#endif