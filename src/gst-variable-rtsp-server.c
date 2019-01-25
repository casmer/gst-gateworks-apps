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
#define VERSION "1.4"
#endif

#include <ecode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gst/gst.h>
#include <gst/gstelement.h>
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
#define DEFAULT_ENABLE_VARIABLE_MODE  "1"
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
enum {pipeline=0, source, encoder, protocol, sink};
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
	char * userpipeline;
	gboolean connected;	      /* Flag to see if this is in use */
	gboolean enable_variable_mode; /* enable automatic rate adjustment */
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
	gchar* command_pipe;   /* command-pipe */
	gchar* status_pipe;   /* status-pipe */
	gint command_pipe_fd; /* command-pipe file descriptor */
	gint status_pipe_fd;  /* status-pipe file descriptor */
	FILE * status_pipe_stream;  /* status-pipe file descriptor */
};

/* Global Variables */

static unsigned int g_dbg = 0;

#define dbg(lvl, fmt, ...) _dbg (__func__, __LINE__, lvl, fmt "\n", ##__VA_ARGS__)
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


static void setup_status_pipe_if_needed(struct stream_info *si)
{
	if (si->status_pipe != NULL && si->status_pipe_fd ==0)
	{
		dbg(4, "opening status pipe ");
		si->status_pipe_fd = open(si->status_pipe, O_WRONLY );;
		if (si->status_pipe_fd <= 0)
		{
			dbg(4, "Failed to open status pipe file descriptor ");
		} else {
			printf("status pipe fd = %d\n", si->status_pipe_fd);
		}
		dbg(4, "Creating status pipe stream ");
		si->status_pipe_stream = fdopen(si->status_pipe_fd, "w");
		if (si->status_pipe_stream == 0)
		{
			dbg(4, "Failed to open status pipe stream ");
		}
		dbg(4, "status pipe ready ");
	}
}

static void send_status_pipe_msg(struct stream_info *si,
		  const char* msg_type, const char *fmt, ...)
{
	setup_status_pipe_if_needed(si);
	va_list ap;
	va_start(ap, fmt);
	if (si->status_pipe_stream)
	{
		fprintf(si->status_pipe_stream, "msg{\ntype:%s,\ndata:{\n", msg_type);
		vfprintf(si->status_pipe_stream, fmt, ap);
		fprintf(si->status_pipe_stream, "\n}}\n");
		fflush(si->status_pipe_stream);
	} else {
		printf("status-reply: {");
		vprintf(fmt, ap);
		printf("}");
		fflush(stdout);
	}

	va_end(ap);

}

static void do_command_setparam(struct stream_info *si,
	char action[256],
	char elementName[256],
	char padName[256],
	char paramName[256],
	char paramValue[256]
	)
{

	dbg(0, "action: [%s], element: [%s], padName: [%s], paramName: [%s], paramValue: [%s]",
				action, elementName, padName, paramName, paramValue);
	double value;
	GstElement* gstElement = NULL;
	if (si->connected==FALSE)
	{
		dbg(0, "not connected, nothing to do.");
		send_status_pipe_msg(si, "setparam", "%s:%s:%s:%s:not streaming", elementName, padName, paramName, paramValue);
		return;
	} else {
		dbg(0, "Connected! Lets do this!");
	}

	dbg(0, "getting pipeline");
	/* Check gstreamer pipeline */

	if (si->stream[pipeline] == NULL)
	{
		send_status_pipe_msg(si, "setparam", "%s:%s:%s:%s:not streaming", elementName, padName, paramName, paramValue);
		dbg(0, "ERROR: pipeline element not populated, is there a stream running?");
		return;
	}

	/* Get gstreamer element to modify*/
	dbg(0, "get element: [%s]", elementName);
	gstElement = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]), elementName);

	if (gstElement == NULL)
	{
		send_status_pipe_msg(si, "setparam", "%s:%s:%s:%s:not streaming", elementName, padName, paramName, paramValue);
		dbg(0, "ERROR: Failed getting the element name = %s", elementName);
	}
	/* Get pad to set value one*/
	dbg(0, "get pad");
	GstPad *gstPad = NULL;

	if (strcmp(padName, "")==0)
	{
		dbg(1, "No Pad provided, setting element property.");

	} else {
		gstPad	= gst_element_get_static_pad(gstElement, padName);
		if(gstPad == NULL)
		{
			gst_object_unref(gstElement);
			send_status_pipe_msg(si, "setparam", "%s:%s:%s:%s:not streaming", elementName, padName, paramName, paramValue);
			dbg(0, "Failed to get static pad %s", padName);
			return;
		}
	}
	/* Set pad parameter */
	GValue param = G_VALUE_INIT;
	g_value_init(&param, G_TYPE_DOUBLE);
	dbg(0, "parsing value");
	value = atof(paramValue);
	g_value_set_double(&param, value);
	dbg(0, "setting property");
	if (gstPad!=NULL){
		g_object_set_property((GObject*)gstPad, paramName, &param);

		/* Finished so unreference the pad*/
		gst_object_unref(gstPad);
	} else
	{
		g_object_set_property((GObject*)gstElement, paramName, &param);
	}
	send_status_pipe_msg(si, "setparam", "%s:%s:%s:%s:not streaming", elementName, padName, paramName, paramValue);
	/* Finished so unreference the element*/
	gst_object_unref(gstElement);

}


/* obj will be NULL if we're printing properties of pad template pads */
static void print_object_properties_info (struct stream_info *si, GstElement *element)
{
	GObject * obj;
	GObjectClass * obj_class;
	GParamSpec **property_specs;
	guint num_properties, i;
	gboolean readable;
	obj = G_OBJECT(element);
	obj_class = G_OBJECT_GET_CLASS(element);
	property_specs = g_object_class_list_properties (obj_class, &num_properties);
	char buffer[500];
	char responsebuffer[8192]="";

	sprintf(responsebuffer, "classname: %s,\n",G_OBJECT_CLASS_NAME (obj_class));
	for (i = 0; i < num_properties; i++)
	{
		GValue value = { 0, };
		GParamSpec *param = property_specs[i];
		GType owner_type = param->owner_type;

		/* We're printing pad properties */
		if (obj == NULL && (owner_type == G_TYPE_OBJECT
				|| owner_type == GST_TYPE_OBJECT || owner_type == GST_TYPE_PAD))
		  continue;

		g_value_init (&value, param->value_type);

		sprintf(buffer,"%s:", g_param_spec_get_name (param));
		gboolean print_value=TRUE;
		readable = ! !(param->flags & G_PARAM_READABLE);
		if (readable && obj != NULL) {
		  g_object_get_property (obj, param->name, &value);
		} else {
		  /* if we can't read the property value, assume it's set to the default
		   * (which might not be entirely true for sub-classes, but that's an
		   * unlikely corner-case anyway) */
		  g_value_reset (&value);
		  continue;
		}
		switch (G_VALUE_TYPE (&value)) {
		  case G_TYPE_STRING:
		  {
			const char *string_val = g_value_get_string (&value);
			if (string_val == NULL)
				sprintf(buffer + strlen(buffer), "null");
			else
				sprintf(buffer + strlen(buffer),"\"%s\"", string_val);
			break;
		  }
		  case G_TYPE_BOOLEAN:
		  {
			gboolean bool_val = g_value_get_boolean (&value);

			sprintf(buffer + strlen(buffer), "%s", bool_val ? "true" : "false");
			break;
		  }
		  case G_TYPE_ULONG:
		  {
			gulong ulong_val = g_value_get_ulong (&value);

			sprintf(buffer + strlen(buffer), "%lu",ulong_val);

			break;
		  }
		  case G_TYPE_LONG:
		  {
			glong long_value = g_value_get_long (&value);
			sprintf(buffer + strlen(buffer), "%ld",long_value);

			break;
		  }
		  case G_TYPE_UINT:
		  {
			uint uint_value = g_value_get_uint (&value);
			sprintf(buffer + strlen(buffer), "%u",uint_value);
			break;
		  }
		  case G_TYPE_INT:
		  {
			  sprintf(buffer + strlen(buffer), "%d", g_value_get_int (&value));

			break;
		  }
		  case G_TYPE_UINT64:
		  {
			  sprintf(buffer + strlen(buffer), "%" G_GUINT64_FORMAT, g_value_get_uint64 (&value));

			break;
		  }
		  case G_TYPE_INT64:
		  {
			  sprintf(buffer + strlen(buffer), "%" G_GINT64_FORMAT, g_value_get_int64 (&value));

			break;
		  }
		  case G_TYPE_FLOAT:
		  {
			  sprintf(buffer + strlen(buffer), "%15.7g", g_value_get_float (&value));

			break;
		  }
		  case G_TYPE_DOUBLE:
		  {
			  sprintf(buffer + strlen(buffer), "%15.7g", g_value_get_double (&value));

			break;
		  }
		  case G_TYPE_CHAR:
		  case G_TYPE_UCHAR:
		  {
			//do nothing
			  print_value=FALSE;
			  break;
		  }
		  default:
			if (G_IS_PARAM_SPEC_ENUM (param))
			{

				GEnumValue *values;
				guint j = 0;
				gint enum_value;
				const gchar *value_nick = "";
				values = G_ENUM_CLASS (g_type_class_ref (param->value_type))->values;
				enum_value = g_value_get_enum (&value);

				while (values[j].value_name)
				{
				  if (values[j].value == enum_value)
				  value_nick = values[j].value_nick;
				  j++;
				}
				sprintf(buffer + strlen(buffer), "[%d]%s", enum_value, value_nick);

			  /* g_type_class_unref (ec); */
			} else if (GST_IS_PARAM_SPEC_FRACTION (param))
			{
				sprintf(buffer + strlen(buffer), "%d/%d",gst_value_get_fraction_numerator (&value),
					  gst_value_get_fraction_denominator (&value));
			} else
			{
			  //do nothing
				print_value=FALSE;
			}
			break;
		}
		if (print_value==TRUE)
		{
			sprintf(responsebuffer+strlen(responsebuffer),"%s,\n", buffer);
		}
		g_value_reset (&value);
	}
	if (num_properties == 0)
	{
		dbg (4, "No properties\n");
	}
	if (responsebuffer[strlen(responsebuffer)-2]==',')
	{
		responsebuffer[strlen(responsebuffer)-2]=0;
	}
	send_status_pipe_msg(si,"pipelineobjectprops", responsebuffer);

	g_free (property_specs);
}

#define do_command_status(si) _do_command_status(__func__, __LINE__, si)
static void _do_command_status(const char* functionname, int line, struct stream_info *si)
{



	char buffer[4096] = "";
	sprintf(buffer, "source:\"%s:%d\",\n", functionname, line);
	sprintf(buffer + strlen(buffer), "num_cli:%d,\n", si->num_cli);
	sprintf(buffer + strlen(buffer), "connected:%s,\n", si->connected == TRUE ? "true" : "false");

	sprintf(buffer + strlen(buffer), "config_interval:%d,\n", si->config_interval);
	sprintf(buffer + strlen(buffer), "idr:%d,\n", si->idr);
	if (si->enable_variable_mode == TRUE)
	{
		sprintf(buffer + strlen(buffer), "enable_variable_mode:true,\n");

		sprintf(buffer + strlen(buffer), "steps:%d,\n", si->steps);
		sprintf(buffer + strlen(buffer), "curr_quant_lvl:%d,\n", si->curr_quant_lvl);
		sprintf(buffer + strlen(buffer), "min_quant_lvl:%d,\n", si->min_quant_lvl);
		sprintf(buffer + strlen(buffer), "max_quant_lvl:%d,\n", si->max_quant_lvl);
		sprintf(buffer + strlen(buffer), "curr_bitrate:%d,\n", si->curr_bitrate);
		sprintf(buffer + strlen(buffer), "min_bitrate:%d,\n", si->min_bitrate);
		sprintf(buffer + strlen(buffer), "max_bitrate:%d,\n", si->max_bitrate);

	} else {
		sprintf(buffer + strlen(buffer), "enable_variable_mode:false,\n");
	}

	sprintf(buffer + strlen(buffer), "periodic_msg_rate:%d", si->msg_rate);

	send_status_pipe_msg(si,"status", buffer);

}
static void do_command_print_bin(struct stream_info *si) {
	if (si->connected==FALSE)
	{
		dbg(0, "not connected, nothing to do.");
		return;
	} else {
		dbg(0, "Connected! Lets do this!");
	}

	if (GST_IS_BIN (si->stream[pipeline])) {
		GstIterator *it = gst_bin_iterate_elements (GST_BIN (si->stream[pipeline]));
		GValue item = G_VALUE_INIT;
		gboolean done = FALSE;

		while (!done) {
				switch (gst_iterator_next (it, &item)) {
					case GST_ITERATOR_OK:
					{
						GstElement *element = g_value_get_object(&item);
						print_object_properties_info(si, element);
						printf("\n");
						g_value_reset (&item);
						break;
					}
					case GST_ITERATOR_RESYNC:
					{
						gst_iterator_resync (it);
						break;
					}
					case GST_ITERATOR_ERROR:
					{
						done = TRUE;
						break;
					}
					case GST_ITERATOR_DONE:
					{
						done = TRUE;
						break;
					}
			}
		}
		gst_iterator_free (it);
	}
}
static void process_command(char command[256], int len, struct stream_info *si)
{
	//mutex_lock();
	dbg(0, " Command: %s", command);

	char params[5][256] = {"","","","",""};
	char catme[2];
	catme[1]=0;

	int delimeter=0;
	for (int i =0; i < len; i++)
	{
		catme[0] = command[i];
		if (strcmp(catme, ":")==0 || strcmp(catme, "\n")==0)
			delimeter++;
		else if (delimeter <= 4)
		{
			strcat(params[delimeter], catme);
		} else {
			dbg(0, "extra character: %s", catme);
		}
	}

	if (strcmp(params[0], "setparam")==0)
	{
		dbg(4, "calling do_command_setparam");
		if (delimeter < 4)
		{
		dbg(0, "not enough values: %d", delimeter);
		} else {
			do_command_setparam(si,params[0], params[1], params[2], params[3], params[4] );
		}
	} else if (strcmp(params[0], "printbin")==0)
	{
		dbg(4, "calling do_command_print_bin");
		do_command_print_bin(si);
	} else if (strcmp(params[0], "status")==0)
	{
		dbg(4, "calling do_command_status");
		do_command_status(si);
	} else {
		dbg(0, "Undefined action [%s]", params[0]);
	}

}

static gboolean reader(struct stream_info *si)
{
	char    ch_buffer[256];
	int pos=0;
	int result= 1;
	while(result>0){
		char    ch;
		result = read (si->command_pipe_fd, &ch, 1);
		if (result > 0 && ch != '\n') {
			ch_buffer[pos++]=ch;
		} else if (pos >= 256) {
			dbg(0, "Invalid command! [%s]", ch_buffer);
			pos = 0;
			strcpy(ch_buffer, "");
		} else if (pos>0)
		{
			dbg(0, "command is %s", ch_buffer);

			/*execute command!*/
			process_command(ch_buffer, pos, si);
			pos = 0;
			strcpy(ch_buffer, "");
		}
		if (result<=0){
			return TRUE;
		}
	}
	return TRUE;
}

static gboolean periodic_msg_handler(struct stream_info *si)
{
	dbg(4, "called");

	if (si->connected == FALSE) {
		dbg(2, "Destroying 'periodic message' handler");
		return FALSE;
	}

	if (si->msg_rate > 0) {
		GstStructure *stats;
		g_print("### MSG BLOCK ###\n");
		g_print("Number of Clients    : %d\n", si->num_cli);
		if (si->enable_variable_mode)
		{
			g_print("Current Quant Level  : %d\n", si->curr_quant_lvl);
			g_print("Current Bitrate Level: %d\n", si->curr_bitrate);
			g_print("Step Factor          : %d\n", (si->curr_bitrate) ?
				((si->max_bitrate - si->min_bitrate) / si->steps) :
				((si->max_quant_lvl - si->min_quant_lvl) / si->steps));
		}
		g_object_get(G_OBJECT(si->stream[protocol]), "stats", &stats,
			     NULL);
		if (stats) {
			g_print("General RTSP Stats   : %s\n",
				gst_structure_to_string(stats));
			gst_structure_free (stats);
		}

		g_print("\n");
	} else {
		dbg(2, "Destroying 'periodic message' handler");
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
	dbg(4, "called");

	si->media = media;

	g_print("[%d]Configuring pipeline...\n", si->num_cli);

	si->stream[pipeline] = gst_rtsp_media_get_element(media);
	si->stream[source] = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]),
						 "source0");
	si->stream[encoder] = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]),
						  "enc0");
	si->stream[protocol] = gst_bin_get_by_name(
		GST_BIN(si->stream[pipeline]), "pay0");

	if((si->stream[source]))
	{
		/* Modify v4l2src Properties */
		g_print("Setting input device=%s\n", si->video_in);
		g_object_set(si->stream[source], "device", si->video_in, NULL);
	} else
	{
		g_printerr("Couldn't get source (source0) pipeline element\n");
	}
	if((si->stream[encoder]))
	{
		/* Modify imxvpuenc_h264 Properties */

		if (si->enable_variable_mode)
		{
			g_print("Setting encoder bitrate=%d\n", si->curr_bitrate);
			g_object_set(si->stream[encoder], "bitrate", si->curr_bitrate, NULL);
			g_print("Setting encoder quant-param=%d\n", si->curr_quant_lvl);
			g_object_set(si->stream[encoder], "quant-param", si->curr_quant_lvl,
					 NULL);
			g_object_set(si->stream[encoder], "idr-interval", si->idr, NULL);
		} else
		{
			dbg(2,"Not setting any encoder properties");
		}

	} else {
		g_printerr("Couldn't get encoder (enc0) pipeline element\n");
	}

	if((si->stream[protocol]))
	{
		/* Modify rtph264pay Properties */
		g_print("Setting rtp config-interval=%d\n",(int) si->config_interval);
		g_object_set(si->stream[protocol], "config-interval",
			     si->config_interval, NULL);
	} else
	{
		g_printerr("Couldn't get protocol (pay0) pipeline element\n");
	}

	if (si->num_cli == 1) {
		/* Create Msg Event Handler */

		if (si->msg_rate>0)
		{
			dbg(4, "Creating 'periodic message' handler");
			g_timeout_add(si->msg_rate * 1000,
					  (GSourceFunc)periodic_msg_handler, si);
		} else {
			dbg(4, "'periodic message' handler disabled");
		}
	}
}

/**
 * change_quant
 * handle changing of quant-levels
 */
static void change_quant(struct stream_info *si)
{
	dbg(4, "called");
	if (si->stream[encoder])
	{
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
}

/**
 * change_bitrate
 * handle changing of bitrates
 */
static void change_bitrate(struct stream_info *si)
{
	dbg(4, "called");
	if (si->stream[encoder])
	{
		int c = si->curr_bitrate;
		int step = (si->max_bitrate - si->min_bitrate) / si->steps;

		/* Change bitrate based on # of clients * step factor */
		si->curr_bitrate = si->max_bitrate - ((si->num_cli - 1) * step);

		/* cap to min bitrate levels */
		if (si->curr_bitrate < si->min_bitrate) {
			dbg(3, "Snapping bitrate to %d", si->min_bitrate);
			si->curr_bitrate = si->min_bitrate;
		}

		if (si->curr_bitrate != c) {
			g_print("[%d]Changing bitrate from %d to %d\n", si->num_cli, c,
				si->curr_bitrate);
			g_object_set(si->stream[encoder], "bitrate", si->curr_bitrate,
					 NULL);
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
	dbg(4, "called");

	si->num_cli--;

	g_print("[%d]Client is closing down\n", si->num_cli);
	if (si->num_cli == 0) {
		dbg(3, "Connection terminated");
		si->connected = FALSE;

		if ((si->stream[pipeline]))
		{
			dbg(4, "deleting pipeline");
			gst_element_set_state(si->stream[pipeline],GST_STATE_NULL);
			gst_object_unref(si->stream[pipeline]);
			si->stream[pipeline] = NULL;
		}
		if ((si->stream[source]))
		{
			dbg(4, "deleting source");
			gst_object_unref(si->stream[source]);
			si->stream[source] = NULL;
		}
		if ((si->stream[encoder]))
		{
			dbg(4, "deleting encoder");
			gst_object_unref(si->stream[encoder]);
			si->stream[encoder] = NULL;
		}
		if ((si->stream[protocol]))
		{
			dbg(4, "deleting protocol");
			gst_object_unref(si->stream[protocol]);
			si->stream[protocol] = NULL;
		}

		/* Created when first new client connected */
		dbg(4, "freeing si->stream");
		free(si->stream);
		do_command_status(si);
	} else {
		if (si->enable_variable_mode)
		{
			if (si->curr_bitrate)
				change_bitrate(si);
			else
				change_quant(si);
		}
	}
	dbg(4, "exiting");
}

/**
 * new_client_handler
 * Called by rtsp server on a new client connection
 */
static void new_client_handler(GstRTSPServer *server, GstRTSPClient *client,
			       struct stream_info *si)
{
	dbg(4, "called");

	/* Used to initiate the media-configure callback */
	static gboolean first_run = TRUE;

	si->num_cli++;
	g_print("[%d]A new client has connected\n", si->num_cli);
	dbg(0, "*************Cliented connected! [%d]", si->num_cli);
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
			dbg(2, "Creating 'media-configure' signal handler");
			g_signal_connect(si->factory, "media-configure",
					 G_CALLBACK(media_configure_handler),
					 si);
		}


	} else {
		if (si->enable_variable_mode)
		{
			if (si->curr_bitrate)
				change_bitrate(si);
			else
				change_quant(si);
		}
	}

	/* Create new client_close_handler */
	dbg(2, "Creating 'closed' signal handler");
	g_signal_connect(client, "closed",
			 G_CALLBACK(client_close_handler), si);
	do_command_status(si);
	dbg(4, "leaving");
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
		.enable_variable_mode = atoi(DEFAULT_ENABLE_VARIABLE_MODE),
		.steps = atoi(DEFAULT_STEPS) - 1,
		.min_quant_lvl = atoi(MIN_QUANT_LVL),
		.max_quant_lvl = atoi(MAX_QUANT_LVL),
		.curr_quant_lvl = atoi(CURR_QUANT_LVL),
		.min_bitrate = 1,
		.max_bitrate = atoi(CURR_BR),
		.curr_bitrate = atoi(CURR_BR),
		.msg_rate = 5,
		.command_pipe = NULL,
		.status_pipe = NULL,
		.command_pipe_fd = 0,
		.status_pipe_fd = 0,
		.status_pipe_stream = NULL,
		.userpipeline = NULL,

	};

	char *port = (char *) DEFAULT_PORT;
	char *mount_point = (char *) DEFAULT_MOUNT_POINT;
	char *src_element = (char *) DEFAULT_SRC_ELEMENT;

	/* Launch pipeline shouldn't exceed LAUNCH_MAX bytes of characters */
	char launch[LAUNCH_MAX];

	/* User Arguments */
	const struct option long_opts[] = {
		{"help",                 no_argument,       0, '?'},
		{"version",              no_argument,       0, 'v'},
		{"debug",                required_argument, 0, 'd'},
		{"mount-point",          required_argument, 0, 'm'},
		{"port",                 required_argument, 0, 'p'},
		{"user-pipeline",        required_argument, 0, 'u'},
		{"src-element",          required_argument, 0, 's'},
		{"video-in",             required_argument, 0, 'i'},
		{"enable-variable-mode", required_argument, 0, 'e' },
		{"steps",                required_argument, 0,  0 },
		{"min-bitrate",          required_argument, 0,  0 },
		{"max-bitrate",          required_argument, 0, 'b'},
		{"max-quant-lvl",        required_argument, 0,  0 },
		{"min-quant-lvl",        required_argument, 0, 'l'},
		{"config-interval",      required_argument, 0, 'c'},
		{"idr",                  required_argument, 0, 'a'},
		{"msg-rate",             required_argument, 0, 'r'},
		{"command-pipe",         required_argument, 0,  0 },
		{"status-pipe",          required_argument, 0,  0 },
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
		" --enable-variable-mode   -e - Enable variable bit rate logic, 0 = off, 1 = on \n"
		" (default: " DEFAULT_ENABLE_VARIABLE_MODE " \n"
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
		" (default: 5s, 0 disables)\n\n"
		" --command-pipe,       - Pipe for pad property commands for IPC"
		" --status-pipe,        - Pipe for command status replies for IPC"
		"Examples:\n"
		" 1. Capture using imxv4l2videosrc, changes quality:\n"
		"\tgst-variable-rtsp-server -s imxv4l2videosrc\n"
		"\n"
		" 2. Create RTSP server out of user created pipeline:\n"
		"\tgst-variable-rtsp-server -u \"videotestsrc ! imxvpuenc_h264"
		" ! rtph264pay name=pay0 pt=96\"\n"
		" 3. Create Same RTSP server out of user created pipeline, but with IPC:\n"
				"\tgst-variable-rtsp-server -u \"videotestsrc ! imxvpuenc_h264"
				" ! rtph264pay name=pay0 pt=96\" --command-pipe \"/tmp/rtsp-control\"\n"
				" --status-pipe \"/tmp/rtsp-status\"\n"
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
				dbg(1, "set steps to: %d", info.steps);
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

				dbg(1, "set min bitrate to: %d",
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
				dbg(1, "set max quant to: %d",
				    info.max_quant_lvl);
			} else if (strcmp(long_opts[opt_ndx].name,
						  "command-pipe") == 0) {
					info.command_pipe = optarg;
					dbg(1, "set command pipe to: %s",
						info.command_pipe);
			} else if (strcmp(long_opts[opt_ndx].name,
						  "status-pipe") == 0) {
					info.status_pipe = optarg;
					dbg(1, "set status pipe to: %s",
						info.status_pipe);
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
			dbg(1, "set debug level to: %d", g_dbg);
			break;
		case 'm': /* Mount Point */
			mount_point = optarg;
			dbg(1, "set mount point to: %s", mount_point);
			break;
		case 'p': /* Port */
			port = optarg;
			dbg(1, "set port to: %s", port);
			break;
		case 'u': /* User Pipeline*/
			info.userpipeline = optarg;
			dbg(1, "set user pipeline to: %s", info.userpipeline);
			break;
		case 's': /* Video source element */
			src_element = optarg;
			dbg(1, "set source element to: %s", src_element);
			break;
		case 'i': /* Video in parameter */
			info.video_in = optarg;
			dbg(1, "set video in to: %s", info.video_in);
			break;
		case 'e': /* enable-variable-mode */
			if (atoi(optarg) > 0)
			{
				info.enable_variable_mode = TRUE;
			} else
			{
				info.enable_variable_mode = FALSE;
			}
			dbg(1, "set enable-variable-mode to: %d", info.enable_variable_mode);
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
			dbg(1, "set max bitrate to: %d", info.max_bitrate);
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
			dbg(1, "set min quant lvl to: %d",
			    info.min_quant_lvl);
			break;
		case 'c': /* config-interval */
			info.config_interval = atoi(optarg);
			dbg(1, "set rtsp config interval to: %d",
			    info.config_interval);
			break;
		case 'a': /* idr frame interval */
			info.idr = atoi(optarg);
			dbg(1, "set idr interval to: %d", info.idr);
			break;
		case 'r': /* how often to display messages at */
			info.msg_rate = atoi(optarg);
			dbg(1, "set msg rate to: %d", info.msg_rate);
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
	if (info.userpipeline)
		snprintf(launch, LAUNCH_MAX, "( %s )", info.userpipeline);
	else
		snprintf(launch, LAUNCH_MAX, "%s name=source0 ! "
			 STATIC_SINK_PIPELINE,
			 src_element
			 );
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

	/* Configure Callbacks and signals */

	/* setup command pipe if specfied*/
	if (info.command_pipe!=NULL)
	{
		mkfifo(info.command_pipe, 0666);
		info.command_pipe_fd = open(info.command_pipe, O_RDONLY | O_NONBLOCK );;


		if (info.status_pipe!=NULL)
		{
			dbg(4, "Creating status pipe [%s]", info.status_pipe);
			mkfifo(info.status_pipe, 0666);

		}
		g_timeout_add(100, (GSourceFunc)reader, &info);
	}

	/* Create new client handler (Called on new client connect) */

	dbg(2, "Creating 'client-connected' signal handler");
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
