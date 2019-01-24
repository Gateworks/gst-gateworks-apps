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
 * gstreamer rtph264pay:
 *  - config-interval: SPS and PPS Insertion Interval
 * h264:
 *  - idr_interval - interval between IDR frames
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
#define DEFAULT_SRC_ELEMENT     "v4l2src"
#define STATIC_SINK_PIPELINE			\
	" imxipuvideotransform name=caps0 !"	\
	" imxvpuenc_h264 name=enc0 !"		\
	" rtph264pay name=pay0 pt=96"

/* Default quality 'steps' */
#define DEFAULT_STEPS "5"

/* max number of chars. in a pipeline */
#define LAUNCH_MAX 8192

/**
 * imxvpuenc_h264:
 *  - bitrate: Bitrate to use, in kbps
 *             (0 = no bitrate control; constant quality mode is used)
 *  - quant-param: Constant quantization quality parameter
 *                 (ignored if bitrate is set to a nonzero value)
 *                 Please note that '0' is the 'best' quality.
 */
#define MIN_BR  "0"	     /* The min value "bitrate" to (0 = VBR)*/
#define MAX_BR  "4294967295" /* Max as defined by imxvpuenc_h264 */
#define CURR_BR "10000"      /* Default to 10mbit/s */

#define MIN_QUANT_LVL  "0"   /* Minimum quant-param for h264 */
#define MAX_QUANT_LVL  "51"  /* Maximum quant-param for h264 */
#define CURR_QUANT_LVL MIN_QUANT_LVL

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
	gchar *video_in;	      /* Video in device */
	gint config_interval;	      /* RTP Send Config Interval */
	gint idr;		      /* Interval betweeen IDR frames */
	gint steps;		      /* Steps to scale quality at */
	gint min_quant_lvl;	      /* Min Quant Level */
	gint max_quant_lvl;	      /* Max Quant Level */
	gint curr_quant_lvl;	      /* Current Quant Level */
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
	dbg(4, "called\n");

	if (si->connected == FALSE) {
		dbg(2, "Destroying 'periodic message' handler\n");
		return FALSE;
	}

	if (si->msg_rate > 0) {
		GstStructure *stats;
		g_print("### MSG BLOCK ###\n");
		g_print("Number of Clients    : %d\n", si->num_cli);
		g_print("Current Quant Level  : %d\n", si->curr_quant_lvl);
		g_print("Current Bitrate Level: %d\n", si->curr_bitrate);
		g_print("Step Factor          : %d\n", (si->curr_bitrate) ?
			((si->max_bitrate - si->min_bitrate) / si->steps) :
			((si->max_quant_lvl - si->min_quant_lvl) / si->steps));

		g_object_get(G_OBJECT(si->stream[protocol]), "stats", &stats,
			     NULL);
		if (stats) {
			g_print("General RTSP Stats   : %s\n",
				gst_structure_to_string(stats));
			gst_structure_free (stats);
		}

		g_print("\n");
	} else {
		dbg(2, "Destroying 'periodic message' handler\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * media_configure_handler
 * Setup pipeline when the stream is first configured
 */
static void media_configure_handler(GstRTSPMediaFactory *factory,
				    GstRTSPMedia *media, struct stream_info *si)
{
	dbg(4, "called\n");

	si->media = media;

	g_print("[%d]Configuring pipeline...\n", si->num_cli);

	si->stream[pipeline] = gst_rtsp_media_get_element(media);
	si->stream[source] = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]),
						 "source0");
	si->stream[caps] = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]),
					       "caps0");
	si->stream[encoder] = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]),
						  "enc0");
	si->stream[protocol] = gst_bin_get_by_name(
		GST_BIN(si->stream[pipeline]), "pay0");

	if(!(si->stream[source] &&
	     si->stream[caps] &&
	     si->stream[encoder] &&
	     si->stream[protocol])) {
		g_printerr("Couldn't get pipeline elements\n");
		exit(-ECODE_PIPE);
	}

	/* Modify v4l2src Properties */
	g_print("Setting input device=%s\n", si->video_in);
	g_object_set(si->stream[source], "device", si->video_in, NULL);

	/* Modify imxvpuenc_h264 Properties */
	g_print("Setting encoder bitrate=%d\n", si->curr_bitrate);
	g_object_set(si->stream[encoder], "bitrate", si->curr_bitrate, NULL);
	g_print("Setting encoder quant-param=%d\n", si->curr_quant_lvl);
	g_object_set(si->stream[encoder], "quant-param", si->curr_quant_lvl,
		     NULL);
	g_object_set(si->stream[encoder], "idr-interval", si->idr, NULL);

	/* Modify rtph264pay Properties */
	g_print("Setting rtp config-interval=%d\n",(int) si->config_interval);
	g_object_set(si->stream[protocol], "config-interval",
		     si->config_interval, NULL);

	if (si->num_cli == 1) {
		/* Create Msg Event Handler */
		dbg(2, "Creating 'periodic message' handler\n");
		g_timeout_add(si->msg_rate * 1000,
			      (GSourceFunc)periodic_msg_handler, si);
	}
}

/**
 * change_quant
 * handle changing of quant-levels
 */
static void change_quant(struct stream_info *si)
{
	dbg(4, "called\n");

	gint c = si->curr_quant_lvl;
	int step = (si->max_quant_lvl - si->min_quant_lvl) / si->steps;

	/* Change quantization based on # of clients * step factor */
	/* It's OK to scale from min since lower val means higher qual */
	si->curr_quant_lvl = ((si->num_cli - 1) * step) + si->min_quant_lvl;

	/* Cap to max quant level */
	if (si->curr_quant_lvl > si->max_quant_lvl)
		si->curr_quant_lvl = si->max_quant_lvl;

	if (si->curr_quant_lvl != c) {
		g_print("[%d]Changing quant-lvl from %d to %d\n", si->num_cli,
			c, si->curr_quant_lvl);
		g_object_set(si->stream[encoder], "quant-param",
			     si->curr_quant_lvl, NULL);
	}
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
		g_object_set(si->stream[encoder], "bitrate", si->curr_bitrate,
			     NULL);
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

		if (!(si->stream[pipeline] && si->stream[source] &&
		      si->stream[caps] && si->stream[encoder] &&
		      si->stream[protocol])) {
			gst_element_set_state(si->stream[pipeline],
					      GST_STATE_NULL);

			gst_object_unref(si->stream[source]);
			gst_object_unref(si->stream[caps]);
			gst_object_unref(si->stream[encoder]);
			gst_object_unref(si->stream[protocol]);
			gst_object_unref(si->stream[pipeline]);
		}
		/* Created when first new client connected */
		free(si->stream);
	} else {
		if (si->curr_bitrate)
			change_bitrate(si);
		else
			change_quant(si);
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
		if (si->curr_bitrate)
			change_bitrate(si);
		else
			change_quant(si);
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
		.video_in = "/dev/video0",
		.config_interval = atoi(DEFAULT_CONFIG_INTERVAL),
		.idr = atoi(DEFAULT_IDR_INTERVAL),
		.steps = atoi(DEFAULT_STEPS) - 1,
		.min_quant_lvl = atoi(MIN_QUANT_LVL),
		.max_quant_lvl = atoi(MAX_QUANT_LVL),
		.curr_quant_lvl = atoi(CURR_QUANT_LVL),
		.min_bitrate = 1,
		.max_bitrate = atoi(CURR_BR),
		.curr_bitrate = atoi(CURR_BR),
		.msg_rate = 5,
	};

	char *port = (char *) DEFAULT_PORT;
	char *mount_point = (char *) DEFAULT_MOUNT_POINT;
	char *src_element = (char *) DEFAULT_SRC_ELEMENT;
	char *caps_filter = NULL;
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
		{"user-pipeline",    required_argument, 0, 'u'},
		{"src-element",      required_argument, 0, 's'},
		{"video-in",         required_argument, 0, 'i'},
		{"caps-filter",      required_argument, 0, 'f'},
		{"steps",            required_argument, 0,  0 },
		{"min-bitrate",      required_argument, 0,  0 },
		{"max-bitrate",      required_argument, 0, 'b'},
		{"max-quant-lvl",    required_argument, 0,  0 },
		{"min-quant-lvl",    required_argument, 0, 'l'},
		{"config-interval",  required_argument, 0, 'c'},
		{"idr",              required_argument, 0, 'a'},
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
		" --user-pipeline,   -u - User supplied pipeline. Note the\n"
		"                         below options are NO LONGER"
		" applicable.\n"
		" --src-element,     -s - Gstreamer source element. Must have\n"
		"                         a 'device' property"
		" (default: " DEFAULT_SRC_ELEMENT ")\n"
		" --video-in,        -i - Input Device (default: /dev/video0)\n"
		" --caps-filter,     -f - Caps filter between src and\n"
		"                         video transform (default: None)\n"
		" --steps,              - Steps to get to 'worst' quality"
		" (default: " DEFAULT_STEPS ")\n"
		" --max-bitrate,     -b - Max bitrate cap, 0 == VBR"
		" (default: " CURR_BR ")\n"
		" --min-bitrate,        - Min bitrate cap"
		" (default: 1)\n"
		" --max-quant-lvl,      - Max quant-level cap"
		" (default: " MAX_QUANT_LVL ")\n"
		" --min-quant-lvl,   -l - Min quant-level cap"
		" (default: " MIN_QUANT_LVL ")\n"
		" --config-interval, -c - Interval to send rtp config"
		" (default: 2s)\n"
		" --idr              -a - Interval between IDR Frames"
		" (default: " DEFAULT_IDR_INTERVAL ")\n"
		" --msg-rate,        -r - Rate of messages displayed"
		" (default: 5s)\n\n"
		"Examples:\n"
		" 1. Capture using imxv4l2videosrc, changes quality:\n"
		"\tgst-variable-rtsp-server -s imxv4l2videosrc\n"
		"\n"
		" 2. Create RTSP server out of user created pipeline:\n"
		"\tgst-variable-rtsp-server -u \"videotestsrc ! imxvpuenc_h264"
		" ! rtph264pay name=pay0 pt=96\"\n"
		;

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
			if (strcmp(long_opts[opt_ndx].name, "steps") == 0) {
				/* Change steps to internal usage of it */
				info.steps = atoi(optarg) - 1;
				dbg(1, "set steps to: %d\n", info.steps);
			} else if (strcmp(long_opts[opt_ndx].name,
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
			} else if (strcmp(long_opts[opt_ndx].name,
					  "max-quant-lvl") == 0) {
				info.max_quant_lvl = atoi(optarg);
				if (info.max_quant_lvl > atoi(MAX_QUANT_LVL)) {
					g_print("Maximum quant-lvl is "
						MAX_QUANT_LVL
						".\n");
					info.max_quant_lvl =
						atoi(MAX_QUANT_LVL);
				} else if (info.max_quant_lvl <
					   atoi(MIN_QUANT_LVL)) {
					g_print("Minimum quant-lvl is "
						MIN_QUANT_LVL
						".\n");
					info.max_quant_lvl =
						atoi(MIN_QUANT_LVL);
				}
				dbg(1, "set max quant to: %d\n",
				    info.max_quant_lvl);
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
		case 'u': /* User Pipeline*/
			user_pipeline = optarg;
			dbg(1, "set user pipeline to: %s\n", user_pipeline);
			break;
		case 's': /* Video source element */
			src_element = optarg;
			dbg(1, "set source element to: %s\n", src_element);
			break;
		case 'i': /* Video in parameter */
			info.video_in = optarg;
			dbg(1, "set video in to: %s\n", info.video_in);
			break;
		case 'f': /* caps filter */
			caps_filter = optarg;
			dbg(1, "set caps filter to: %s\n", caps_filter);
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
		case 'l':
			info.min_quant_lvl = atoi(optarg);
			if (info.min_quant_lvl > atoi(MAX_QUANT_LVL)) {
				g_print("Maximum quant-lvl is " MAX_QUANT_LVL
					".\n");
				info.min_quant_lvl = atoi(MAX_QUANT_LVL);
			} else if (info.min_quant_lvl < atoi(MIN_QUANT_LVL)) {
				g_print("Minimum quant-lvl is " MIN_QUANT_LVL
					".\n");
				info.min_quant_lvl = atoi(MIN_QUANT_LVL);
			}

			info.curr_quant_lvl = info.min_quant_lvl;
			dbg(1, "set min quant lvl to: %d\n",
			    info.min_quant_lvl);
			break;
		case 'c': /* config-interval */
			info.config_interval = atoi(optarg);
			dbg(1, "set rtsp config interval to: %d\n",
			    info.config_interval);
			break;
		case 'a': /* idr frame interval */
			info.idr = atoi(optarg);
			dbg(1, "set idr interval to: %d\n", info.idr);
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

	/* Validate inputs */
	if (info.max_quant_lvl < info.min_quant_lvl) {
		g_printerr("Max Quant level must be"
			   "greater than Min Quant level\n");
		return -ECODE_ARGS;
	}

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
	if (user_pipeline)
		snprintf(launch, LAUNCH_MAX, "( %s )", user_pipeline);
	else
		snprintf(launch, LAUNCH_MAX, "%s name=source0 ! %s%s"
			 STATIC_SINK_PIPELINE,
			 src_element,
			 (caps_filter) ? caps_filter : "",
			 (caps_filter) ? " ! " : "");
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
	if (!user_pipeline) {
		dbg(2, "Creating 'client-connected' signal handler\n");
		g_signal_connect(info.server, "client-connected",
				 G_CALLBACK(new_client_handler), &info);
	}

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
