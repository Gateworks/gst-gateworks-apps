/**
 * Copyright (C) 2015 Pushpal Sidhu <psidhu@gateworks.com>
 *
 * Filename: gst-variable-rtsp-server.c
 * Author: Pushpal Sidhu <psidhu@gateworks.com>
 * Created: Tue May 19 14:29:23 2015 (-0700)
 * Version: 1.0
 * Last-Updated: Fri Jan 15 14:22:59 2016 (-0800)
 *           By: Pushpal Sidhu
 *
 * Compatibility: ARCH=arm && proc=imx6
 */

/**
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef VERSION
#define VERSION "1.4"
#endif

#include <ecode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib.h>

/**
 * rtsp-server:
 *  - port: Server port
 *  - mount_point: Server mount point
 *  - host: local host name
 *  - src_element: GStreamer element to act as a source
 *  - sink pipeline: Static pipeline to take source to rtsp server
 */
#define DEFAULT_CONFIG_INTERVAL "2"
#define DEFAULT_IDR_INTERVAL    "0"
#define DEFAULT_PORT            "9099"
#define DEFAULT_MOUNT_POINT     "/stream"
#define DEFAULT_HOST            "127.0.0.1"

/* Default quality 'steps' */
#define DEFAULT_STEPS "5"

/* max number of chars. in a pipeline */
#define LAUNCH_MAX 8192

/**
 * imxvpuenc_h264:
 *  - bitrate: Bitrate to use, in kbps
 *             (0 = no bitrate control; constant quality mode is used)
 *
 * v4l2h264enc:
 *  The encoder driver's controls are exposed via V4L2 controls in the
 *  'extra-controls' property. For CODA960 h264 we use the following:
 *  - video_bitrate: Bitrate to use, in kbps
 *  - 
 */
#define MIN_BR  "0"	     /* The min value "bitrate" to (0 = VBR)*/
#define MAX_BR  "4294967295" /* Max as defined by imxvpuenc_h264 */
#define CURR_BR "10000"      /* Default to 10mbit/s */

/**
 * Source and Sink must always be positioned as such. Elements can be added
 * in between, however.
 */
enum {pipeline=0, source, caps, encoder, protocol, sink};
#define NUM_ELEM (source + sink)

struct stream_info {
	gint num_cli;		      /* Number of clients */
	GMainLoop *main_loop;	      /* Main loop pointer */
	GstRTSPServer *server;	      /* RTSP Server */
	GstRTSPServer *client;	      /* RTSP Client */
	GstRTSPMountPoints *mounts;   /* RTSP Mounts */
	GstRTSPMediaFactory *factory; /* RTSP Factory */
	GstRTSPMedia *media;	      /* RTSP Media */
	GstElement **stream;	      /* Array of elements */
	gboolean connected;	      /* Flag to see if this is in use */
	gint steps;		      /* Steps to scale quality at */
	gint min_bitrate;	      /* Min Bitrate */
	gint max_bitrate;	      /* Max Bitrate */
	gint curr_bitrate;	      /* Current Bitrate */
	gint msg_rate;		      /* In Seconds */
};

/* Global Variables */
static unsigned int g_dbg = 0;

#define dbg(lvl, fmt, ...) _dbg (__func__, __LINE__, lvl, fmt, ##__VA_ARGS__)
void _dbg(const char *func, unsigned int line,
	  unsigned int lvl, const char *fmt, ...)
{
	if (g_dbg >= lvl) {
		va_list ap;
		printf("[%d]:%s:%d - ", lvl, func, line);
		va_start(ap, fmt);
		vprintf(fmt, ap);
		fflush(stdout);
		va_end(ap);
	}
}

static gboolean periodic_msg_handler(struct stream_info *si)
{
	const gchar *name = g_ascii_strdown(G_OBJECT_TYPE_NAME(si->stream[encoder]), -1);
	dbg(4, "called\n");

	if (si->connected == FALSE) {
		dbg(2, "Destroying 'periodic message' handler\n");
		return FALSE;
	}

	if (si->msg_rate > 0) {
		GstStructure *stats;
		g_print("### MSG BLOCK ###\n");
		g_print("Number of Clients    : %d\n", si->num_cli);
		g_print("Current Bitrate Level: %d\n", si->curr_bitrate);
		g_print("Step Factor          : %d\n",
			(si->max_bitrate - si->min_bitrate) / si->steps);

		g_object_get(G_OBJECT(si->stream[protocol]), "stats", &stats,
			     NULL);
		if (stats) {
			g_print("General RTSP Stats   : %s\n",
				gst_structure_to_string(stats));
			gst_structure_free (stats);
		}

		if (strstr(name, "v4l2h264enc") != NULL) {
			GstStructure *extra_controls;
			g_object_get(si->stream[encoder], "extra-controls", &extra_controls, NULL);
			g_print("extra-controls=%s\n", gst_structure_to_string(extra_controls));
		}

		g_print("\n");
	} else {
		dbg(2, "Destroying 'periodic message' handler\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * setup_encoder
 * Properly Sets up an encoder based on what encoder is being used
 */
static void setup_encoder(GstElement *enc, const gchar *name,
			  struct stream_info *si)
{
	gchar *str;
	GstStructure *extra_controls;
	si->stream[encoder] = enc;

	/* setup for specific encoders */
	if (strstr(name, "imxvpuenc_h264") != NULL) {
		g_print("Setting encoder bitrate=%d\n", si->curr_bitrate);
		g_object_set(si->stream[encoder], "bitrate", si->curr_bitrate, NULL);
	}
	else if (strstr(name, "v4l2h264enc") != NULL) {

		g_object_get(si->stream[encoder], "extra-controls",
			     &extra_controls, NULL);

		if (extra_controls == NULL) {
			str = g_strdup_printf("controls,"
					      "h264_profile=4,"
					      "video_bitrate=%d",si->curr_bitrate);
			extra_controls = gst_structure_from_string(str, NULL);
			g_free(str);
		} else {
			gst_structure_set(extra_controls,
				  "video_bitrate", G_TYPE_INT, si->curr_bitrate,
				  "h264_profile", G_TYPE_INT, 4,
				  NULL);
		}

		g_print("Setting encoder extra-controls=%s\n",
		       gst_structure_to_string(extra_controls));
		g_object_set(si->stream[encoder], "extra-controls",
			     extra_controls, NULL);
		gst_structure_free(extra_controls);
	}
}

/**
 * setup_payload
 * Sets up payloads if setup is needed
 */
static void setup_payload(GstElement *pay, const gchar *name,
			  struct stream_info *si)
{
	si->stream[protocol] = pay;
}

/**
 * setup_elements
 * Called on each element and if necessary hands the element off
 * to be configured
 */
static void setup_elements(const GValue * item, gpointer user_data)
{
	GstElement *elem = g_value_get_object(item);
	const gchar *name = g_ascii_strdown(G_OBJECT_TYPE_NAME(elem), -1);

	/* call appropriate setup function */
	if (strstr(name, "enc") != NULL)
		setup_encoder(elem, name, user_data);
	else if (strstr(name, "pay") != NULL)
		setup_payload(elem, name, user_data);
}

/**
 * media_configure_handler
 * Setup pipeline when the stream is first configured
 */
static void media_configure_handler(GstRTSPMediaFactory *factory,
				    GstRTSPMedia *media, struct stream_info *si)
{
	GstIterator *iter;

	dbg(4, "called\n");

	si->media = media;

	g_print("[%d]Configuring pipeline...\n", si->num_cli);

	si->stream[pipeline] = gst_rtsp_media_get_element(media);

	/* Iterate through pipeline and setup elements*/
	iter = gst_bin_iterate_elements(GST_BIN(si->stream[pipeline]));
	gst_iterator_foreach(iter, setup_elements, si);
	gst_iterator_free(iter);

	if (si->num_cli == 1) {
		/* Create Msg Event Handler */
		dbg(2, "Creating 'periodic message' handler\n");
		g_timeout_add(si->msg_rate * 1000,
			      (GSourceFunc)periodic_msg_handler, si);
	}

	printf("%s: encoder=%s, payload=%s\n", __func__,
	       G_OBJECT_TYPE_NAME(si->stream[encoder]),
	       G_OBJECT_TYPE_NAME(si->stream[protocol]));
}

/**
 * change_bitrate
 * handle changing of bitrates
 */
static void change_bitrate(struct stream_info *si)
{
	dbg(4, "called\n");

	int c = si->curr_bitrate;
	int step = (si->max_bitrate - si->min_bitrate) / si->steps;
	const gchar *name = g_ascii_strdown(G_OBJECT_TYPE_NAME(si->stream[encoder]), -1);
	GstStructure *extra_controls;

	/* Change bitrate based on # of clients * step factor */
	si->curr_bitrate = si->max_bitrate - ((si->num_cli - 1) * step);

	/* cap to min bitrate levels */
	if (si->curr_bitrate < si->min_bitrate) {
		dbg(3, "Snapping bitrate to %d\n", si->min_bitrate);
		si->curr_bitrate = si->min_bitrate;
	}

	if (si->curr_bitrate != c) {
		g_print("[%d]Changing bitrate from %d to %d\n", si->num_cli, c,
			si->curr_bitrate);
		if (strstr(name, "imxvpuenc_h264") != NULL) {
			g_object_set(si->stream[encoder], "bitrate",
				     si->curr_bitrate, NULL);
		}
		else if (strstr(name, "v4l2h264enc") != NULL) {
			g_object_get(si->stream[encoder], "extra-controls",
				     &extra_controls, NULL);
			gst_structure_set(extra_controls, "video_bitrate",
					  G_TYPE_INT, si->curr_bitrate, NULL);
			g_object_set(si->stream[encoder], "extra-controls",
				     extra_controls, NULL);
			gst_structure_free(extra_controls);
		}
	}
}

/**
 * client_close_handler
 * This is called upon a client leaving. Free's stream data (if last client),
 * decrements a count, free's client resources.
 */
static void client_close_handler(GstRTSPClient *client, struct stream_info *si)
{
	dbg(4, "called\n");

	si->num_cli--;

	g_print("[%d]Client is closing down\n", si->num_cli);
	if (si->num_cli == 0) {
		dbg(3, "Connection terminated\n");
		si->connected = FALSE;

		/* Created when first new client connected */
		free(si->stream);
	} else {
		change_bitrate(si);
	}
}

/**
 * new_client_handler
 * Called by rtsp server on a new client connection
 */
static void new_client_handler(GstRTSPServer *server, GstRTSPClient *client,
			       struct stream_info *si)
{
	dbg(4, "called\n");

	/* Used to initiate the media-configure callback */
	static gboolean first_run = TRUE;

	si->num_cli++;
	g_print("[%d]A new client has connected\n", si->num_cli);
	si->connected = TRUE;

	/* Create media-configure handler */
	if (si->num_cli == 1) {	/* Initial Setup */
		/* Free if no more clients (in close handler) */
		si->stream = malloc(sizeof(GstElement *) * NUM_ELEM);

		/**
		 * Stream info is required, which is only
		 * available on the first connection. Stream info is created
		 * upon the first connection and is never destroyed after that.
		 */
		if (first_run == TRUE) {
			dbg(2, "Creating 'media-configure' signal handler\n");
			g_signal_connect(si->factory, "media-configure",
					 G_CALLBACK(media_configure_handler),
					 si);
		}
	} else {
		change_bitrate(si);
	}

	/* Create new client_close_handler */
	dbg(2, "Creating 'closed' signal handler\n");
	g_signal_connect(client, "closed",
			 G_CALLBACK(client_close_handler), si);

	first_run = FALSE;
}

int main (int argc, char *argv[])
{
	GstStateChangeReturn ret;

	struct stream_info info = {
		.num_cli = 0,
		.connected = FALSE,
		.steps = atoi(DEFAULT_STEPS) - 1,
		.min_bitrate = 1,
		.max_bitrate = atoi(CURR_BR),
		.curr_bitrate = atoi(CURR_BR),
		.msg_rate = 5,
	};

	char *port = (char *) DEFAULT_PORT;
	char *mount_point = (char *) DEFAULT_MOUNT_POINT;
	char *user_pipeline = NULL;
	/* Launch pipeline shouldn't exceed LAUNCH_MAX bytes of characters */
	char launch[LAUNCH_MAX];

	/* User Arguments */
	const struct option long_opts[] = {
		{"help",             no_argument,       0, '?'},
		{"version",          no_argument,       0, 'v'},
		{"debug",            required_argument, 0, 'd'},
		{"mount-point",      required_argument, 0, 'm'},
		{"port",             required_argument, 0, 'p'},
		{"steps",            required_argument, 0, 's'},
		{"min-bitrate",      required_argument, 0,  0 },
		{"max-bitrate",      required_argument, 0, 'b'},
		{"msg-rate",         required_argument, 0, 'r'},
		{ /* Sentinel */ }
	};
	char *arg_parse = "?hvd:m:p:u:s:i:f:b:l:c:a:r:";
	const char *usage =
		"Usage: gst-variable-rtsp-server [OPTIONS]\n\n"
		"Options:\n"
		" --help,            -? - This usage\n"
		" --version,         -v - Program Version: " VERSION "\n"
		" --debug,           -d - Debug Level (default: 0)\n"
		" --mount-point,     -m - What URI to mount"
		" (default: " DEFAULT_MOUNT_POINT ")\n"
		" --port,            -p - Port to sink on"
		" (default: " DEFAULT_PORT ")\n"
		" --steps,           -s - Steps to get to 'worst' quality"
		" (default: " DEFAULT_STEPS ")\n"
		" --max-bitrate,     -b - Max bitrate cap, 0 == VBR"
		" (default: " CURR_BR ")\n"
		" --min-bitrate,        - Min bitrate cap"
		" (default: 1)\n"
		" --msg-rate,        -r - Rate of messages displayed"
		" (default: 5s)\n\n"
		"Examples:\n"
		" - Create RTSP server out of user created pipeline:\n"
		"\tgst-variable-rtsp-server \"videotestsrc ! v4l2h264enc"
		" ! rtph264pay name=pay0 pt=96\"\n"
		;

	/* Ensure that there are command arguments */
	if (argc == 1) {
		puts(usage);
		return -ECODE_ARGS;
	}

	/* Init GStreamer */
	gst_init(&argc, &argv);

	/* Parse Args */
	while (TRUE) {
		int opt_ndx;
		int c = getopt_long(argc, argv, arg_parse, long_opts, &opt_ndx);

		if (c < 0)
			break;

		switch (c) {
		case 0: /* long-opts only parsing */
			if (strcmp(long_opts[opt_ndx].name,
					"min-bitrate") == 0) {
				info.min_bitrate = atoi(optarg);
				if (info.min_bitrate > atoi(MAX_BR)) {
					g_print("Maximum bitrate is "
						MAX_BR ".\n");
					info.min_bitrate = atoi(MAX_BR);
				} else if (info.min_bitrate <= atoi(MIN_BR)) {
					g_print("Minimum bitrate is 1\n");
					info.min_bitrate = 1;
				}

				dbg(1, "set min bitrate to: %d\n",
				    info.min_bitrate);
			} else {
				puts(usage);
				return -ECODE_ARGS;
			}
			break;
		case 'h': /* Help */
		case '?':
			puts(usage);
			return ECODE_OKAY;
		case 'v': /* Version */
			puts("Program Version: " VERSION);
			return ECODE_OKAY;
		case 'd':
			g_dbg = atoi(optarg);
			dbg(1, "set debug level to: %d\n", g_dbg);
			break;
		case 'm': /* Mount Point */
			mount_point = optarg;
			dbg(1, "set mount point to: %s\n", mount_point);
			break;
		case 'p': /* Port */
			port = optarg;
			dbg(1, "set port to: %s\n", port);
			break;
		case 's': /* Steps */
			info.steps = atoi(optarg) - 1;
			dbg(1, "set steps to: %d\n", info.steps);
			break;
		case 'b': /* Max Bitrate */
			info.max_bitrate = atoi(optarg);
			if (info.max_bitrate > atoi(MAX_BR)) {
				g_print("Maximum bitrate is " MAX_BR ".\n");
				info.max_bitrate = atoi(MAX_BR);
			} else if (info.max_bitrate < atoi(MIN_BR)) {
				g_print("Minimum bitrate is " MIN_BR ".\n");
				info.max_bitrate = atoi(MIN_BR);
			}

			info.curr_bitrate = info.max_bitrate;
			dbg(1, "set max bitrate to: %d\n", info.max_bitrate);
			break;
		case 'r': /* how often to display messages at */
			info.msg_rate = atoi(optarg);
			dbg(1, "set msg rate to: %d\n", info.msg_rate);
			break;
		default: /* Default - bad arg */
			puts(usage);
			return -ECODE_ARGS;
		}

	}

	/* Grab user pipeline */
	user_pipeline = argv[argc-1];

	/* Validate inputs */
	if ((info.max_bitrate + 1) < info.min_bitrate) {
		g_printerr("Max bitrate must be greater than min bitrate\n");
		return -ECODE_ARGS;
	}

	if (info.steps < 1) {
		/* Because we subtract 1 off of user input of steps,
		 * we must account for it here when reporting to user
		 */
		g_printerr("Steps must be 2 or greater\n");
		return -ECODE_ARGS;
	}

	if (strlen(g_strstrip(user_pipeline)) == 0) {
		g_printerr("A pipeline must be specified\n");
		return -ECODE_ARGS;
	}

	/* Configure RTSP */
	info.server = gst_rtsp_server_new();
	if (!info.server) {
		g_printerr("Could not create RTSP server\n");
		return -ECODE_RTSP;
	}
	g_object_set(info.server, "service", port, NULL);

	/* Map URI mount points to media factories */
	info.mounts = gst_rtsp_server_get_mount_points(info.server);
	info.factory = gst_rtsp_media_factory_new();
	if (!info.factory) {
		g_printerr("Could not create RTSP server\n");
		return -ECODE_RTSP;
	}
	/* Share single pipeline with all clients */
	gst_rtsp_media_factory_set_shared(info.factory, TRUE);

	/* Source Pipeline */
	snprintf(launch, LAUNCH_MAX, "( %s )", user_pipeline);
	g_print("Pipeline set to: %s...\n", launch);
	gst_rtsp_media_factory_set_launch(info.factory, launch);

	/* Connect pipeline to the mount point (URI) */
	gst_rtsp_mount_points_add_factory(info.mounts, mount_point,
					  info.factory);

	/* Create GLIB MainContext */
	info.main_loop = g_main_loop_new(NULL, FALSE);

	/* Attach server to default maincontext */
	ret = gst_rtsp_server_attach(info.server, NULL);
	if (ret == FALSE) {
		g_printerr("Unable to attach RTSP server\n");
		return -ECODE_RTSP;
	}

	/* Configure Callbacks */
	/* Create new client handler (Called on new client connect) */
	printf("Creating 'client-connected' signal handler\n");
	g_signal_connect(info.server, "client-connected",
		         G_CALLBACK(new_client_handler), &info);

	/* Run GBLIB main loop until it returns */
	g_print("Stream ready at rtsp://" DEFAULT_HOST ":%s%s\n",
		port, mount_point);
	g_main_loop_run(info.main_loop);

	/* Cleanup */
	g_main_loop_unref(info.main_loop);
	g_object_unref(info.factory);
	g_object_unref(info.media);
	g_object_unref(info.mounts);
	return ECODE_OKAY;
}

/* gst-variable-rtsp-server.c ends here */
