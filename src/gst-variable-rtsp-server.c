/**
 * Copyright (C) 2015 Pushpal Sidhu <psidhu@gateworks.com>
 *
 * Filename: gst-variable-rtsp-server.c
 * Author: Pushpal Sidhu <psidhu@gateworks.com>
 * Created: Tue May 19 14:29:23 2015 (-0700)
 * Version: 1.0
 * Last-Updated: Mon Aug 31 16:49:51 2015 (-0700)
 *           By: Pushpal Sidhu
 *
 * Compatibility: ARCH=arm && proc=imx6
 */

/**
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with gst-variable-rtsp-server. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef VERSION
#define VERSION "1.0"
#endif

#include <ecode.h>

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib.h>

/**
 * gstreamer rtph264pay:
 *  - config-interval: SPS and PPS Insertion Interval
 * rtsp-server:
 *  - port: Server port
 *  - mount_point: Server mount point
 *  - host: local host name
 *  - src_element: GStreamer element to act as a source
 *  - sink pipeline: Static pipeline to take source to rtsp server
 */
#define DEFAULT_CONFIG_INTERVAL "2"
#define DEFAULT_PORT            "9099"
#define DEFAULT_MOUNT_POINT     "/stream"
#define DEFAULT_HOST            "127.0.0.1"
#define DEFAULT_SRC_ELEMENT     "v4l2src"
#define STATIC_SINK_PIPELINE			\
	" imxipuvideotransform name=caps0 !"	\
	" imxvpuenc_h264 name=enc0 !"		\
	" rtph264pay name=pay0 pt=96"

/* max number of chars. in a pipeline */
#define LAUNCH_MAX 1024

/**
 * imxvpuenc_h264:
 *  - bitrate: Bitrate to use, in kbps
 *             (0 = no bitrate control; constant quality mode is used)
 *  - quant-param: Constant quantization quality parameter
 *                 (ignored if bitrate is set to a nonzero value)
 *                 Please note that '0' is the 'best' quality.
 */
#define MIN_BR  "0"	     /* The min value "bitrate" to (0 = VBR)*/
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
	int num_cli;		      /* Number of clients */
	GMainLoop *main_loop;	      /* Main loop pointer */
	GstRTSPServer *server;	      /* RTSP Server */
	GstRTSPServer *client;	      /* RTSP Client */
	GstRTSPMountPoints *mounts;   /* RTSP Mounts */
	GstRTSPMediaFactory *factory; /* RTSP Factory */
	GstRTSPMedia *media;	      /* RTSP Media */
	GstElement **stream;	      /* Array of elements */
	gboolean connected;	      /* Flag to see if this is in use */
	char *video_in;		      /* Video in device */
	int config_interval;	      /* RTP Send Config Interval */
	int capture_mode;	      /* Capture Mode of Camera */
	int min_quant_lvl;	      /* Min Quant Level */
	int max_quant_lvl;	      /* Max Quant Level */
	int curr_quant_lvl;	      /* Current Quant Level */
	int bitrate;		      /* Bitrate */
	int msg_rate;		      /* In Seconds */
};

static gboolean periodic_msg_handler(struct stream_info *si)
{
	if (si->connected == FALSE)
		return FALSE;

	g_object_get(G_OBJECT(si->stream[encoder]), "quant-param",
		     &si->curr_quant_lvl, NULL);

	if (si->msg_rate > 0) {
		GstStructure *stats;

		g_object_get(G_OBJECT(si->stream[protocol]), "stats", &stats,
			     NULL);

		g_print("### MSG BLOCK ###\n");
		g_print("Number of Clients    : %d\n", si->num_cli);
		g_print("Current Quant Level  : %d\n", si->curr_quant_lvl);
		g_print("Current Bitrate Level: %d\n", si->bitrate);
		g_print("RTSP Stats           : %s\n",
			gst_structure_to_string(stats));
		g_print("\n");
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
	g_print("Setting encoder bitrate=%d\n", si->bitrate);
	g_object_set(si->stream[encoder], "bitrate", si->bitrate, NULL);
	g_print("Setting encoder quant-param=%d\n", si->curr_quant_lvl);
	g_object_set(si->stream[encoder], "quant-param", si->curr_quant_lvl,
		     NULL);
	g_object_set(si->stream[encoder], "idr-interval", -1, NULL);

	/* Modify rtph264pay Properties */
	g_print("Setting rtp config-interval=%d\n",(int) si->config_interval);
	g_object_set(si->stream[protocol], "config-interval",
		     si->config_interval, NULL);

	if (si->num_cli == 1)
		/* Create Msg Event Handler */
		g_timeout_add(si->msg_rate * 1000,
			      (GSourceFunc)periodic_msg_handler, si);
}

/**
 * change_quality
 * handle changing of quant-levels
 */
static void change_quality(struct stream_info *si)
{
	/* Don't change quant if bitrate is constant (CBR) */
	if (si->bitrate != 0)
		return;

	/* Change quant-level depending on # of clients connected */
	int c = si->curr_quant_lvl;

	enum {h=1, mh, m, ml, l}; /* Quality indicators */

	switch (si->num_cli) {
	case 0:
	case h:			/* High */
		si->curr_quant_lvl =
			si->min_quant_lvl; /* ~100% quality */
		break;
	case mh:		/* Medium-high */
		si->curr_quant_lvl =
			((si->max_quant_lvl - si->min_quant_lvl) * .25) +
			si->min_quant_lvl; /* ~75% quality */
		break;
	case m:			/* Medium */
		si->curr_quant_lvl =
			((si->max_quant_lvl - si->min_quant_lvl) * .50) +
			si->min_quant_lvl; /* ~50% quality */
		break;
	case ml:		/* Medium-low */
		si->curr_quant_lvl =
			((si->max_quant_lvl - si->min_quant_lvl) * .75) +
			si->min_quant_lvl; /* ~25% quality */
		break;
	case l:			/* Low */
		si->curr_quant_lvl =
			si->max_quant_lvl; /*  ~0% quality */
		break;
	default:
		g_print("Warning: Exceeding supported number of clients\n");
		g_print("Currently at: %d\n", si->num_cli);
	}

	/* Cap to MIN/MAX quant levels */
	if (si->curr_quant_lvl > atoi(MAX_QUANT_LVL))
		si->curr_quant_lvl = atoi(MAX_QUANT_LVL);
	else if (si->curr_quant_lvl < atoi(MIN_QUANT_LVL))
		si->curr_quant_lvl = atoi(MIN_QUANT_LVL);

	g_print("[%d]Changing quant-lvl from %d to %d\n", si->num_cli, c,
		si->curr_quant_lvl);
	g_object_set(si->stream[encoder], "quant-param", si->curr_quant_lvl,
		     NULL);
}

/**
 * client_close_handler
 * This is called upon a client leaving. Free's stream data (if last client),
 * decrements a count, free's client resources.
 */
static void client_close_handler(GstRTSPClient *client, struct stream_info *si)
{
	si->num_cli--;

	g_print("[%d]Client is closing down\n", si->num_cli);
	if (si->num_cli == 0) {
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
	} else
		change_quality(si);
}

/**
 * new_client_handler
 * Called by rtsp server on a new client connection
 */
static void new_client_handler(GstRTSPServer *server, GstRTSPClient *client,
			       struct stream_info *si)
{
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
		if (first_run == TRUE)
			g_signal_connect(si->factory, "media-configure",
					 G_CALLBACK(media_configure_handler),
					 si);
	} else
		change_quality(si);

	/* Create new client_close_handler */
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
		.min_quant_lvl = atoi(MIN_QUANT_LVL),
		.max_quant_lvl = atoi(MAX_QUANT_LVL),
		.curr_quant_lvl = atoi(CURR_QUANT_LVL),
		.bitrate = atoi(MIN_BR),
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
		{"mount-point",      required_argument, 0, 'm'},
		{"port",             required_argument, 0, 'p'},
		{"user-pipeline",    required_argument, 0, 'u'},
		{"src-element",      required_argument, 0, 's'},
		{"video-in",         required_argument, 0, 'i'},
		{"caps-filter",      required_argument, 0, 'f'},
		{"bitrate",          required_argument, 0, 'b'},
		{"max-quant-lvl",    required_argument, 0, 'g'},
		{"min-quant-lvl",    required_argument, 0, 'l'},
		{"config-interval",  required_argument, 0, 'c'},
		{"msg-rate",         required_argument, 0, 'r'},
		{ /* Sentinel */ }
	};
	char *arg_parse = "?hvm:p:u:s:i:f:b:g:l:c:r:";
	const char *usage =
		"Usage: gst-variable-rtsp-server [OPTIONS]\n\n"
		"Options:\n"
		" --help,            -? - This usage\n"
		" --version,         -v - Program Version: " VERSION "\n"
		" --mount-point,     -m - What URI to mount"
		" (default: " DEFAULT_MOUNT_POINT ")\n"
		" --port,            -p - Port to sink on"
		" (default: " DEFAULT_PORT ")\n"
		" --user-pipeline,   -u - User supplied pipeline. Note the\n"
		"                         below options are NO LONGER\n"
		"                         applicable.\n"
		" --src-element,     -s - Gstreamer source element. Must have\n"
		"                         a 'device' property"
		" (default: " DEFAULT_SRC_ELEMENT ")\n"
		" --video-in,        -i - Input Device (default: /dev/video0)\n"
		" --caps-filter,     -f - Caps filter between src and\n"
		"                         video transform (default: None)\n"
		" --config-interval, -c - Interval to send rtp config"
		" (default: 2s)\n"
		" --bitrate,         -b - Min Bitrate in kbps"
		" (default: " MIN_BR ")\n"
		" --max-quant-lvl,   -g - Max Quant-Level"
		" (default: " MAX_QUANT_LVL ")\n"
		" --min-quant-lvl,   -l - Min Quant-Level"
		" (default: " MIN_QUANT_LVL ")\n"
		" --msg-rate,        -r - Rate of messages displayed"
		" (default: 5s)\n\n"
		"Examples:\n"
		" 1. Capture using imxv4l2videosrc, changes quality:\n"
		"\tgst-variable-rtsp-server -s imxv4l2videosrc\n"
		"\n"
		" 2. Create RTSP server out of user created pipeline:\n"
		"\tgst-variable-rtsp-server -u \"videotestsrc ! imxvpuenc_h264"
		" ! rtph264pay pt=96\""
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
		case 0:
			break;
		case 'h': /* Help */
		case '?':
			puts(usage);
			return ECODE_OKAY;
		case 'v': /* Version */
			puts("Program Version: " VERSION);
			return ECODE_OKAY;
		case 'm': /* Mount Point */
			mount_point = optarg;
			break;
		case 'p': /* Port */
			port = optarg;
			break;
		case 'u': /* User Pipeline*/
			user_pipeline = optarg;
			break;
		case 's': /* Video source element */
			src_element = optarg;
			break;
		case 'i': /* Video in parameter */
			info.video_in = optarg;
			break;
		case 'f': /* caps filter */
			caps_filter = optarg;
			break;
		case 'c': /* config-interval */
			info.config_interval = atoi(optarg);
			break;
		case 'b': /* Bitrate */
			info.bitrate = atoi(optarg);
			if (info.bitrate < atoi(MIN_BR)) {
				g_print("Minimum bitrate is " MIN_BR ".\n");
				info.bitrate = atoi(MIN_BR);
			}
			break;
		case 'g': /* Max Quant Level */
			info.max_quant_lvl = atoi(optarg);
			if (info.max_quant_lvl > atoi(MAX_QUANT_LVL)) {
				g_print("Maximum quant-lvl is " MAX_QUANT_LVL
					".\n");
				info.max_quant_lvl = atoi(MAX_QUANT_LVL);
			} else if (info.max_quant_lvl < atoi(MIN_QUANT_LVL)) {
				g_print("Minimum quant-lvl is " MIN_QUANT_LVL
					".\n");
				info.max_quant_lvl = atoi(MIN_QUANT_LVL);
			}
			break;
		case 'l': /* Min Quant Level */
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
			break;
		case 'r': /* how often to display messages at */
			info.msg_rate = atoi(optarg);
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
	if (!user_pipeline)
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
