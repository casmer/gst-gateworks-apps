/**
 * Copyright (C) 2019 Casey Gregoire <caseyg@lalosoft.com>
 *
 * Filename: gst-variable-rtsp-server.c
 * Author: Casey Gregoire <caseyg@lalosoft.com>
 * Version: 1.5
 *           By: Casey Gregoire
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
#define VERSION "1.5"
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
 *  - mountPoint: Server mount point
 *  - host: local host name
 *  - sourceElement: GStreamer element to act as a source
 *  - sink pipeline: Static pipeline to take source to rtsp server
 */
#define DEFAULT_CONFIG_INTERVAL "2"
#define DEFAULT_IDR_INTERVAL    "0"
#define DEFAULT_PORT            "9099"
#define DEFAULT_MOUNT_POINT     "/stream"
#define DEFAULT_HOST            "127.0.0.1"
#define DEFAULT_SRC_ELEMENT     "v4l2src"
#define DEFAULT_ENABLE_VARIABLE_MODE  "1"
#define STATIC_SINK_PIPELINE            \
    " imxipuvideotransform name=caps0 !"    \
    " imxvpuenc_h264 name=enc0 !"        \
    " rtph264pay name=pay0 pt=96"

/// Default quality 'steps'
#define DEFAULT_STEPS "5"

/// max number of chars. in a pipeline
#define LAUNCH_MAX 8192

///Buffer size for responses to a print bin command.
#define RESPONSE_BUFFER_SIZE 8192
///Buffer size for small strings
#define BUFFER_SIZE 500

///command buffer size for strings sent and revieved via the IPC interface.
#define COMMAND_BUFFER_SIZE 4096
/**
 * imxvpuenc_h264:
 *  - bitrate: Bitrate to use, in kbps
 *             (0 = no bitrate control; variable quality mode is used)
 *  - quant-param: Constant quantization quality parameter
 *                 (ignored if bitrate is set to a nonzero value)
 *                 Please note that '0' is the 'best' quality.
 */
/// The min value "bitrate" to (0 = VBR)
#define MIN_BR  "0"
/// Max as defined by imxvpuenc_h264
#define MAX_BR  "4294967295"
/// Default to 10mbit/s
#define DEFAULT_BR "10000"
/// Minimum quant-param for h264
#define MIN_QUANT_LVL  "0"
/// Maximum quant-param for h264
#define MAX_QUANT_LVL  "51"
/// Default quant-param for h264
#define CURR_QUANT_LVL MIN_QUANT_LVL

///string literal for set param command
#define MSG_T_SETPARAM "setparam"
///string literal for element properties command
#define MSG_T_ELEMENTPROPS "elementprops"
///string literal for status command
#define MSG_T_STATUS "status"
///represents no file for fd variables.
#define NO_FILE -1

/**
 * Source and Sink must always be positioned as such. Elements can be added
 * in between, however.
 */
enum {pipeline=0, source, encoder, protocol, sink};
#define NUM_ELEM (source + sink)



struct StreamInfo {
         /// Number of clients
        gint numConnectedClients;
        /// Main loop pointer
        GMainLoop *mainLoop;
        /// RTSP Server
        GstRTSPServer *server;
        /// RTSP Client
        GstRTSPServer *client;
        /// RTSP Mounts
        GstRTSPMountPoints *mounts;
        /// RTSP Factory
        GstRTSPMediaFactory *factory;
        /// RTSP Media
        GstRTSPMedia *media;
        /// Array of elements
        GstElement **stream;
        /// pipeline passed in on the command line, to replace the default pipeline
        char * userpipeline;
        /// Flag to see if this is in use
        gboolean connected;
        /// enable automatic rate adjustment
        gboolean enableVariableMode;
        /// Video in device
        gchar *videoInDevice;
        /// RTP Send Config Interval
        gint configInterval;
        /// Interval betweeen IDR frames
        gint idr;
        /// Steps to scale quality at
        gint steps;
        /// Min Quant Level
        gint minQuantLevel;
        /// Max Quant Level
        gint maxQuantLevel;
        /// Current Quant Level
        gint currentQuantLevel;
        /// Min total Bitrate
        gint minBitrate;
        /// Max total Bitrate
        gint maxBitrate;
        /// Over Cap on Max Bitrate, for VBR with Cap mode
        gint capBitrate;
        /// Current Bitrate
        gint currentBitrate;
        /// Periodic Status Message Rate to send to staus pipe In Seconds
        gint periodicStatusMessageRate;
        /// command-pipe
        gchar* commandPipe;
        /// status-pipe
        gchar* statusPipe;
        /// command-pipe file descriptor
        gint commandPipeFd;
        /// status-pipe file descriptor
        gint statusPipeFd;
        /// status-pipe file descriptor
        FILE * statusPipeStream;
        /// Address pool used to restrict the RTSP stream to a known port range
        GstRTSPAddressPool* rtspAddressPool;
        /// start of range for rtp client addres pool.
        gint rtspPortMin;
        /// end of range for rtp client addres pool.
        gint rtspPortMax;
        /// enable shared pipeline for all clients
        gboolean enableSharedPipeline;
        ///  enable no suspend on pipeline
        gboolean enableNoSuspend;
};

// Global Variables

static unsigned int debugLevel = 0;

#define dbg(lvl, fmt, ...) _dbg (__func__, __LINE__, lvl, fmt "\n", ##__VA_ARGS__)
void _dbg(const char *func, unsigned int line,
      unsigned int lvl, const char *fmt, ...)
{
    if (debugLevel >= lvl)
    {
        va_list ap;
        printf("[%d]:%s:%d - ", lvl, func, line);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        fflush(stdout);
        va_end(ap);
    }
}


static void setupStatusPipeIfNeeded(struct StreamInfo *si)
{
    if (si->statusPipe != NULL && si->statusPipeFd == NO_FILE)
    {
        dbg(4, "opening status pipe ");
        si->statusPipeFd = open(si->statusPipe, O_WRONLY );
        if (si->statusPipeFd <= 0)
        {
            dbg(4, "Failed to open status pipe file descriptor ");
            return;
        }
        else
        {
            printf("status pipe fd = %d\n", si->statusPipeFd);
        }
        dbg(4, "Creating status pipe stream ");
        si->statusPipeStream = fdopen(si->statusPipeFd, "w");
        if (si->statusPipeStream == 0)
        {
            dbg(4, "Failed to open status pipe stream ");
        }
        else
        {
            dbg(4, "status pipe ready ");
        }
    }
}

static void sendStatusPipeMessage(struct StreamInfo *si,
          const char* messageType, const char *fmt, ...)
{
    setupStatusPipeIfNeeded(si);
    va_list ap;
    va_start(ap, fmt);
    if (si->statusPipeStream)
    {
        fprintf(si->statusPipeStream, "msg{\ntype:%s,\ndata:{\n", messageType);
        vfprintf(si->statusPipeStream, fmt, ap);
        fprintf(si->statusPipeStream, "\n}}\n");
        fflush(si->statusPipeStream);
    }
    else
    {
        printf("status-reply: {\n");
        printf( "msg{\ntype:%s,\ndata:{\n", messageType);
        vprintf( fmt, ap);
        printf( "\n}}\n");
        printf("\n}\n");
        fflush(stdout);
    }

    va_end(ap);

}

static void doCommandSetParameter(struct StreamInfo *si,
    char action[256],
    char elementName[256],
    char padName[256],
    char paramName[256],
    char paramValue[256]
    )
{
    char* pEnd;
    dbg(0, "action: [%s], element: [%s], padName: [%s], paramName: [%s], paramValue: [%s]",
                action, elementName, padName, paramName, paramValue);
    double value;
    GstElement* gstElement = NULL;
    if (si->connected==FALSE)
    {
        dbg(0, "not connected, nothing to do.");
        return;
    }
    else
    {
        dbg(0, "Connected! Lets do this!");
    }

    dbg(0, "getting pipeline");
    // Check gstreamer pipeline

    if (si->stream[pipeline] == NULL)
    {
        dbg(0, "ERROR: pipeline element not populated, is there a stream running?");
        return;
    }

    // Get gstreamer element to modify
    dbg(0, "get element: [%s]", elementName);
    gstElement = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]), elementName);

    if (gstElement == NULL)
    {
        dbg(0, "ERROR: Failed getting the element name = %s", elementName);
    }
    // Get pad to set value one
    dbg(0, "get pad");
    GstPad *gstPad = NULL;

    if (strcmp(padName, "")==0)
    {
        dbg(1, "No Pad provided, setting element property.");

    }
    else
    {
        gstPad    = gst_element_get_static_pad(gstElement, padName);
        if(gstPad == NULL)
        {
            gst_object_unref(gstElement);
            dbg(0, "Failed to get static pad %s", padName);
            return;
        }
    }
    // Set pad parameter
    GValue param = G_VALUE_INIT;
    g_value_init(&param, G_TYPE_DOUBLE);
    dbg(0, "parsing value");
    errno = 0;
    value =strtod(paramValue, &pEnd);
    if (errno != 0)
    {
        dbg(0, "invalid value sent for setparam. errno:[%d]; value: [%s] ",errno, paramValue);
    }
    else
    {
        g_value_set_double(&param, value);
        dbg(0, "setting property");
        if (gstPad!=NULL)
        {
            g_object_set_property((GObject*)gstPad, paramName, &param);

            // Finished so unreference the pad
            gst_object_unref(gstPad);
        }
        else
        {
            g_object_set_property((GObject*)gstElement, paramName, &param);
        }
    }
    // Finished so unreference the element
    gst_object_unref(gstElement);

}


/**
 * prints object properties for the giving element, does not recursively do this, just this level.
 * @param si stream info for access to running configuration.
 * @param element element to print the properties of.
 */
static void printObjectPropertiesInfo (struct StreamInfo *si, GstElement *element)
{
    GObject * obj;
    GObjectClass * objClass;
    GParamSpec **propertySpecs;
    guint numProperties, i;
    gboolean readable;

    obj = G_OBJECT(element);
    objClass = G_OBJECT_GET_CLASS(element);
    propertySpecs = g_object_class_list_properties (objClass, &numProperties);
    char buffer[BUFFER_SIZE];
    char responsebuffer[RESPONSE_BUFFER_SIZE]="";

    snprintf(responsebuffer,RESPONSE_BUFFER_SIZE, "classname: %s,\n",G_OBJECT_CLASS_NAME (objClass));
    for (i = 0; i < numProperties; i++)
    {
        GValue value = { 0, };
        GParamSpec *param = propertySpecs[i];
        GType owner_type = param->owner_type;

        // We're printing pad properties
        if (obj == NULL && (owner_type == G_TYPE_OBJECT
                || owner_type == GST_TYPE_OBJECT || owner_type == GST_TYPE_PAD))
          continue;

        g_value_init (&value, param->value_type);

        snprintf(buffer,BUFFER_SIZE,"%s:", g_param_spec_get_name (param));
        gboolean print_value=TRUE;
        readable = ! !(param->flags & G_PARAM_READABLE);
        if (readable && obj != NULL)
        {
          g_object_get_property (obj, param->name, &value);
        }
        else
        {
          // if we can't read the property value, assume it's set to the default
          // (which might not be entirely true for sub-classes, but that's an
          // unlikely corner-case anyway)
          g_value_reset (&value);
          continue;
        }
        switch (G_VALUE_TYPE (&value))
        {
          case G_TYPE_STRING:
          {
            const char *string_val = g_value_get_string (&value);
            if (string_val == NULL)
            {
                snprintf(buffer + strlen(buffer), BUFFER_SIZE,"null");
            }
            else
            {
                snprintf(buffer + strlen(buffer), BUFFER_SIZE,"\"%s\"", string_val);
            }
            break;
          }
          case G_TYPE_BOOLEAN:
          {
            gboolean bool_val = g_value_get_boolean (&value);

            snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%s", bool_val ? "true" : "false");
            break;
          }
          case G_TYPE_ULONG:
          {
            gulong ulong_val = g_value_get_ulong (&value);

            snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%lu",ulong_val);

            break;
          }
          case G_TYPE_LONG:
          {
            glong long_value = g_value_get_long (&value);
            snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%ld",long_value);

            break;
          }
          case G_TYPE_UINT:
          {
            uint uint_value = g_value_get_uint (&value);
            snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%u",uint_value);
            break;
          }
          case G_TYPE_INT:
          {
              snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%d", g_value_get_int (&value));

            break;
          }
          case G_TYPE_UINT64:
          {
              snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%" G_GUINT64_FORMAT, g_value_get_uint64 (&value));

            break;
          }
          case G_TYPE_INT64:
          {
              snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%" G_GINT64_FORMAT, g_value_get_int64 (&value));

            break;
          }
          case G_TYPE_FLOAT:
          {
              snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%15.7g", g_value_get_float (&value));

            break;
          }
          case G_TYPE_DOUBLE:
          {
              snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%15.7g", g_value_get_double (&value));

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
                snprintf(buffer + strlen(buffer), BUFFER_SIZE, "[%d]%s", enum_value, value_nick);

              // g_type_class_unref (ec);
            }
            else if (GST_IS_PARAM_SPEC_FRACTION (param))
            {
                snprintf(buffer + strlen(buffer), BUFFER_SIZE, "%d/%d",gst_value_get_fraction_numerator (&value),
                      gst_value_get_fraction_denominator (&value));
            }
            else
            {
              //do nothing
                print_value=FALSE;
            }
            break;
        }
        if (print_value==TRUE)
        {
            snprintf(responsebuffer+strlen(responsebuffer),RESPONSE_BUFFER_SIZE,"%s,\n", buffer);
        }
        g_value_reset (&value);
    }
    if (numProperties == 0)
    {
        dbg (4, "No properties\n");
    }
    if (responsebuffer[strlen(responsebuffer)-2]==',')
    {
        responsebuffer[strlen(responsebuffer)-2]=0;
    }
    sendStatusPipeMessage(si,MSG_T_ELEMENTPROPS, responsebuffer);

    g_free (propertySpecs);
}

#define doCommandSendStatus(si) _doCommandSendStatus(__func__, __LINE__, si)
static void _doCommandSendStatus(const char* functionname, int line, struct StreamInfo *si)
{
    char buffer[BUFFER_SIZE] = "";
    snprintf(buffer, BUFFER_SIZE, "source:\"%s:%d\",\n", functionname, line);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE, "numConnectedClients:%d,\n", si->numConnectedClients);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE, "connected:%s,\n", si->connected == TRUE ? "true" : "false");

    snprintf(buffer + strlen(buffer), BUFFER_SIZE, "configInterval:%d,\n", si->configInterval);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE, "idr:%d,\n", si->idr);
    if (si->enableVariableMode == TRUE)
    {
        snprintf(buffer + strlen(buffer), BUFFER_SIZE, "enableVariableMode:true,\n");

        snprintf(buffer + strlen(buffer), BUFFER_SIZE, "steps:%d,\n", si->steps);

        snprintf(buffer + strlen(buffer), BUFFER_SIZE, "minQuantLevel:%d,\n", si->minQuantLevel);
        snprintf(buffer + strlen(buffer), BUFFER_SIZE, "maxQuantLevel:%d,\n", si->maxQuantLevel);

        snprintf(buffer + strlen(buffer), BUFFER_SIZE, "minBitrate:%d,\n", si->minBitrate);
        snprintf(buffer + strlen(buffer), BUFFER_SIZE, "maxBitrate:%d,\n", si->maxBitrate);

    }
    else
    {
        snprintf(buffer + strlen(buffer), BUFFER_SIZE, "enableVariableMode:false,\n");
    }
    snprintf(buffer + strlen(buffer), BUFFER_SIZE, "currentBitrate:%d,\n", si->currentBitrate);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE, "currentQuantLevel:%d,\n", si->currentQuantLevel);
    snprintf(buffer + strlen(buffer), BUFFER_SIZE, "periodic_msg_rate:%d", si->periodicStatusMessageRate);

    sendStatusPipeMessage(si,"status", buffer);

}
static void doCommandPrintBin(struct StreamInfo *si)
{
    if (si->connected==FALSE)
    {
        dbg(0, "not connected, nothing to do.");
        return;
    }
    else
    {
        dbg(0, "Connected! Lets do this!");

        if (GST_IS_BIN (si->stream[pipeline]))
        {
            GstIterator *it = gst_bin_iterate_elements (GST_BIN (si->stream[pipeline]));
            GValue item = G_VALUE_INIT;
            gboolean done = FALSE;

            while (!done)
            {
                    switch (gst_iterator_next (it, &item))
                    {
                        case GST_ITERATOR_OK:
                        {
                            GstElement *element = g_value_get_object(&item);
                            printObjectPropertiesInfo(si, element);
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
                            g_printerr("error occurred during gst_iterator_next call.");
                            break;
                        }
                        case GST_ITERATOR_DONE:
                        default:
                        {
                            done = TRUE;
                            break;
                        }
                }
            }
            gst_iterator_free (it);
        }
    }
}
static void processCommand(char command[256], int len, struct StreamInfo *si)
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
        {
            delimeter++;
        }
        else if (delimeter <= 4)
        {
            strcat(params[delimeter], catme);
        }
        else
        {
            dbg(0, "extra character: %s", catme);
        }
    }

    if (strcmp(params[0], "setparam")==0)
    {
        dbg(4, "calling doCommandSetParameter");
        if (delimeter < 4)
        {
            dbg(0, "not enough values: %d", delimeter);
        }
        else
        {
            doCommandSetParameter(si,params[0], params[1], params[2], params[3], params[4] );
        }
    }
    else if (strcmp(params[0], "printbin")==0)
    {
        dbg(4, "calling doCommandPrintBin");
        doCommandPrintBin(si);
    }
    else if (strcmp(params[0], "status")==0)
    {
        dbg(4, "calling doCommandSendStatus");
        doCommandSendStatus(si);
    }
    else
    {
        dbg(0, "Undefined action [%s]", params[0]);
    }

}

static gboolean reader(struct StreamInfo *si)
{
    char    ch_buffer[256];
    int pos=0;
    int result= 1;
    while(result>0)
    {
        char    ch;
        result = read (si->commandPipeFd, &ch, 1);
        if (result > 0 && ch != '\n')
        {
            ch_buffer[pos++]=ch;
        }
        else if (pos >= 256)
        {
            dbg(0, "Invalid command! [%s]", ch_buffer);
            pos = 0;
            strcpy(ch_buffer, "");
        }
        else if (pos>0)
        {
            dbg(0, "command is %s", ch_buffer);

            //execute command!
            processCommand(ch_buffer, pos, si);
            pos = 0;
            strcpy(ch_buffer, "");
        }
    }
    //always return true to this continues to be called.
    return TRUE;
}

/**
 * handler for sending periodic status data to the status pipe.
 * @param si stream info for access to running configuration.
 * @return returns TRUE until the stream is disconnected.
 */
static gboolean periodicMessageHandler(struct StreamInfo *si)
{
    dbg(4, "called");

    if (si->connected == FALSE)
    {
        dbg(2, "Destroying 'periodic message' handler");
        return FALSE;
    }

    if (si->periodicStatusMessageRate > 0)
    {
        GstStructure *stats;
        g_print("### MSG BLOCK ###\n");
        g_print("Number of Clients    : %d\n", si->numConnectedClients);
        g_print("Current Quant Level  : %d\n", si->currentQuantLevel);
        g_print("Current Bitrate Level: %d\n", si->currentBitrate);
        if (si->enableVariableMode)
        {

            g_print("Step Factor          : %d\n", (si->maxBitrate) ?
                ((si->maxBitrate - si->minBitrate) / si->steps) :
                ((si->maxQuantLevel - si->minQuantLevel) / si->steps));
        }
        g_object_get(G_OBJECT(si->stream[protocol]), "stats", &stats,
                 NULL);
        if (stats)
        {
            g_print("General RTSP Stats   : %s\n",
                gst_structure_to_string(stats));
            gst_structure_free (stats);
        }

        g_print("\n");
    }
    else
    {
        dbg(2, "Destroying 'periodic message' handler");
        return FALSE;
    }

    return TRUE;
}

/**
 * Setup pipeline when the stream is first configured
 * @param factory gst provided factory for the pipeline connection being created
 * @param media gst media reference for this connection.
 * @param si stream info for access to running configuration.
 */
static void mediaConfigureHandler(GstRTSPMediaFactory *factory,
                    GstRTSPMedia *media, struct StreamInfo *si)
{
    dbg(4, "called");

    si->media = media;

    g_print("[%d]Configuring pipeline...\n", si->numConnectedClients);

    si->stream[pipeline] = gst_rtsp_media_get_element(media);
    si->stream[source] = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]),
                         "source0");
    si->stream[encoder] = gst_bin_get_by_name(GST_BIN(si->stream[pipeline]),
                          "enc0");
    si->stream[protocol] = gst_bin_get_by_name(
        GST_BIN(si->stream[pipeline]), "pay0");

    if((si->stream[source]))
    {
        // Modify v4l2src Properties
        g_print("Setting input device=%s\n", si->videoInDevice);
        g_object_set(si->stream[source], "device", si->videoInDevice, NULL);
    }
    else
    {
        g_printerr("Couldn't get source (source0) pipeline element\n");
    }
    if((si->stream[encoder]))
    {
        // Modify imxvpuenc_h264 Properties
        g_print("Setting encoder bitrate=%d\n", si->currentBitrate);
        g_object_set(si->stream[encoder], "bitrate", si->currentBitrate, NULL);
        g_print("Setting encoder quant-param=%d\n", si->currentQuantLevel);
        g_object_set(si->stream[encoder], "quant-param", si->currentQuantLevel,
                 NULL);
        g_object_set(si->stream[encoder], "idr-interval", si->idr, NULL);
    }
    else
    {
        g_printerr("Couldn't get encoder (enc0) pipeline element\n");
    }

    if((si->stream[protocol]))
    {
        // Modify rtph264pay Properties
        g_print("Setting rtp config-interval=%d\n",(int) si->configInterval);
        g_object_set(si->stream[protocol], "config-interval",
                 si->configInterval, NULL);
    }
    else
    {
        g_printerr("Couldn't get protocol (pay0) pipeline element\n");
    }

    if (si->numConnectedClients == 1)
    {
        // Create Msg Event Handler

        if (si->periodicStatusMessageRate>0)
        {
            dbg(4, "Creating 'periodic message' handler");
            g_timeout_add(si->periodicStatusMessageRate * 1000,
                      (GSourceFunc)periodicMessageHandler, si);
        }
        else
        {
            dbg(4, "'periodic message' handler disabled");
        }
    }
}

/**
 * handle changing of quant-levels
 * @param si stream info for access to running configuration.
 */
static void changeQuant(struct StreamInfo *si)
{
    dbg(4, "called");
    if (si->stream[encoder] && si->maxQuantLevel > 0)
    {
        gint c = si->currentQuantLevel;
        int step = (si->maxQuantLevel - si->minQuantLevel) / si->steps;

        // Change quantization based on # of clients * step factor
        // It's OK to scale from min since lower val means higher qual
        si->currentQuantLevel = ((si->numConnectedClients - 1) * step) + si->minQuantLevel;

        // Cap to max quant level
        if (si->currentQuantLevel > si->maxQuantLevel)
            si->currentQuantLevel = si->maxQuantLevel;

        if (si->currentQuantLevel != c)
        {
            g_print("[%d]Changing quant-lvl from %d to %d\n", si->numConnectedClients,
                c, si->currentQuantLevel);
            g_object_set(si->stream[encoder], "quant-param",
                     si->currentQuantLevel, NULL);
        }
    }
}

/**
 * handle changing of bitrates
 * @param si stream info for access to running configuration.
 */
static void changeBitrate(struct StreamInfo *si)
{
    dbg(4, "called");
    if (si->stream[encoder])
    {
        int c = si->currentBitrate;
        int step = (si->maxBitrate - si->minBitrate) / si->steps;

        // Change bitrate based on # of clients * step factor
        si->currentBitrate = si->maxBitrate - ((si->numConnectedClients - 1) * step);

        // cap to min bitrate levels
        if (si->currentBitrate < si->minBitrate)
        {
            dbg(3, "Snapping bitrate to %d", si->minBitrate);
            si->currentBitrate = si->minBitrate;
        }

        if (si->currentBitrate != c)
        {
            g_print("[%d]Changing bitrate from %d to %d\n", si->numConnectedClients, c,
                si->currentBitrate);
            g_object_set(si->stream[encoder], "bitrate", si->currentBitrate,
                     NULL);
        }
    }
}

/**
 * This is called upon a client leaving. Free's stream data (if last client),
 * decrements a count, free's client resources.
 * @param client gst client object
 * @param si stream info for access to running configuration.
 */
static void clientCloseHandler(GstRTSPClient *client, struct StreamInfo *si)
{
    dbg(4, "called");

    si->numConnectedClients--;

    g_print("[%d]Client is closing down\n", si->numConnectedClients);
    if (si->numConnectedClients == 0)
    {
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

        // Created when first new client connected
        dbg(4, "freeing si->stream");
        free(si->stream);
        doCommandSendStatus(si);
    }
    else
    {
        if (si->enableVariableMode)
        {
            if (si->maxBitrate)
            {
                changeBitrate(si);
            }
            else
            {
                changeQuant(si);
            }
        }
    }
    dbg(4, "exiting");
}

/**
 * Called by rtsp server on a new client connection
 * @param server rtsp server reference
 * @param client client reference
 * @param si stream info for access to running configuration.
 */
static void newClientHandler(GstRTSPServer *server, GstRTSPClient *client,
                   struct StreamInfo *si)
{
    dbg(4, "called");
    // Used to initiate the media-configure callback
    static gboolean first_run = TRUE;

    si->numConnectedClients++;
    g_print("[%d]A new client has connected\n", si->numConnectedClients);

    GList* sessionList = gst_rtsp_client_session_filter(client, NULL, NULL);

    for (GList* i = sessionList; i != NULL; i = i->next)
    {
        const unsigned RTSP_SESSION_TIMEOUT = 10;
        gst_rtsp_session_set_timeout((GstRTSPSession*)i->data, RTSP_SESSION_TIMEOUT);
        g_object_unref(i->data);
    }

    g_list_free(sessionList);
    dbg(0, "*************Cliented connected! [%d]", si->numConnectedClients);
    si->connected = TRUE;

    // Create media-configure handler
    if (si->numConnectedClients == 1)
    {
        // Initial Setup
        // Freed if no more clients (in close handler)
        si->stream = malloc(sizeof(GstElement *) * NUM_ELEM);


        // Stream info is required, which is only
        // available on the first connection. Stream info is created
        // upon the first connection and is never destroyed after that.
        if (first_run == TRUE)
        {
            dbg(2, "Creating 'media-configure' signal handler");
            g_signal_connect(si->factory, "media-configure",
                     G_CALLBACK(mediaConfigureHandler),
                     si);
        }
    }
    else
    {
        if (si->enableVariableMode)
        {
            if (si->maxBitrate)
            {
                changeBitrate(si);
            }
            else
            {
                changeQuant(si);
            }
        }
    }

    // Create new clientCloseHandler
    dbg(2, "Creating 'closed' signal handler");
    g_signal_connect(client, "closed",
             G_CALLBACK(clientCloseHandler), si);
    doCommandSendStatus(si);
    dbg(4, "leaving");
    first_run = FALSE;
}

/**
 * Main entry point for this program
 * @param argc agrument count
 * @param argv argmuments
 * @return exit code.
 */
int main (int argc, char *argv[])
{
    GstStateChangeReturn ret;
    struct StreamInfo info = {
        .numConnectedClients = 0,
        .connected = FALSE,
        .videoInDevice = "/dev/video0",
        .configInterval = atoi(DEFAULT_CONFIG_INTERVAL),
        .idr = atoi(DEFAULT_IDR_INTERVAL),
        .enableVariableMode = atoi(DEFAULT_ENABLE_VARIABLE_MODE),
        .steps = atoi(DEFAULT_STEPS) - 1,
        .minQuantLevel = atoi(MIN_QUANT_LVL),
        .maxQuantLevel = atoi(MIN_QUANT_LVL), //Default to min, to disable the adjustment
        .currentQuantLevel = atoi(CURR_QUANT_LVL),
        .minBitrate = 1,
        .maxBitrate = atoi(MIN_BR),
        .currentBitrate = atoi(DEFAULT_BR),
        .periodicStatusMessageRate = 5,
        .commandPipe = NULL,
        .statusPipe = NULL,
        .commandPipeFd = NO_FILE,
        .statusPipeFd = NO_FILE,
        .statusPipeStream = NULL,
        .userpipeline = NULL,
        .rtspPortMin = 0,
        .rtspPortMax = 0,
        .enableSharedPipeline = FALSE,
        .enableNoSuspend = FALSE,

    };

    char *port = (char *) DEFAULT_PORT;
    char *mountPoint = (char *) DEFAULT_MOUNT_POINT;
    char *sourceElement = (char *) DEFAULT_SRC_ELEMENT;

    // Launch pipeline shouldn't exceed LAUNCH_MAX bytes of characters
    char launch[LAUNCH_MAX];

    // User Arguments
    const struct option long_opts[] = {
        {"help",                   no_argument,       0, '?'},
        {"version",                no_argument,       0, 'v'},
        {"debug",                  required_argument, 0, 'd'},
        {"mount-point",            required_argument, 0, 'm'},
        {"port",                   required_argument, 0, 'p'},
        {"client-port-min",        required_argument, 0,  0 },
        {"client-port-max",        required_argument, 0,  0 },
        {"user-pipeline",          required_argument, 0, 'u'},
        {"src-element",            required_argument, 0, 's'},
        {"video-in",               required_argument, 0, 'i'},
        {"enable-variable-mode",   no_argument,       0, 'e' },
        {"steps",                  required_argument, 0,  0 },
        {"min-bitrate",            required_argument, 0,  0 },
        {"max-bitrate",            required_argument, 0, 'b'},
        {"cap-bitrate",            required_argument, 0,  0 },
        {"max-quant-lvl",          required_argument, 0,  0 },
        {"min-quant-lvl",          required_argument, 0, 'l'},
        {"config-interval",        required_argument, 0, 'c'},
        {"idr",                    required_argument, 0, 'a'},
        {"msg-rate",               required_argument, 0, 'r'},
        {"command-pipe",           required_argument, 0,  0 },
        {"status-pipe",            required_argument, 0,  0 },
        {"enable-shared-pipeline", no_argument,       0,  0 },
        {"enable-no-suspend",      no_argument,       0,  0 },
        {} // Sentinel
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
        " --client-port-min     - port range low end for client connections"
        " --client-port-max     - port range high end for client connections"
        "                         Must specify a min and a max, otherwise the"
        "                          program will report an error and exit."
        " (default: " DEFAULT_PORT ")\n"
        " --user-pipeline,   -u - User supplied pipeline. Note the\n"
        "                         below options are NO LONGER"
        " applicable.\n"
        " --src-element,     -s - Gstreamer source element. Must have\n"
        "                         a 'device' property"
        " (default: " DEFAULT_SRC_ELEMENT ")\n"
        " --video-in,        -i - Input Device (default: /dev/video0)\n"
        " --enable-variable-mode   -e - Enable variable bit rate logic\n"
        " (default: " DEFAULT_ENABLE_VARIABLE_MODE " \n"
        " --steps,              - Steps to get to 'worst' quality"
        " (default: " DEFAULT_STEPS ")\n"
        " --max-bitrate,     -b - Max bitrate cap, 0 == VBR"
        " (default: " DEFAULT_BR ")\n"
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
        " --enable-shared-pipeline - use a single pipeline for all clients"
        " --enable-no-suspend   - start pipeline with 'GST_RTSP_SUSPEND_MODE_NONE' set"
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

    // Init GStreamer
    gst_init(&argc, &argv);

    // Parse Args
    while (TRUE)
    {
        int opt_ndx;
        int c = getopt_long(argc, argv, arg_parse, long_opts, &opt_ndx);

        if (c < 0)
            break;

        switch (c)
        {
        case 0: // long-opts only parsing
            if (strcmp(long_opts[opt_ndx].name, "steps") == 0)
            {
                // Change steps to internal usage of it
                info.steps = atoi(optarg) - 1;
                dbg(1, "set steps to: %d", info.steps);
            }
            else if (strcmp(long_opts[opt_ndx].name,
                    "cap-bitrate") == 0)
            {
                info.capBitrate = atoi(optarg);
                if (info.capBitrate > atoi(MAX_BR))
                {
                    g_print("bitrate cap is "
                        MAX_BR ".\n");
                    info.capBitrate = atoi(MAX_BR);
                }
                else if (info.capBitrate <= atoi(MIN_BR))
                {
                    g_print("cap bitrate is " MAX_BR "\n");
                    info.capBitrate = atoi(MAX_BR);
                }

                dbg(1, "set cap bitrate to: %d",
                    info.capBitrate);
            }
            else if (strcmp(long_opts[opt_ndx].name,
                    "min-bitrate") == 0)
            {
                info.minBitrate = atoi(optarg);
                if (info.minBitrate > atoi(MAX_BR))
                {
                    g_print("Maximum bitrate is "
                        MAX_BR ".\n");
                    info.minBitrate = atoi(MAX_BR);
                }
                else if (info.minBitrate <= atoi(MIN_BR))
                {
                    g_print("Minimum bitrate is 1\n");
                    info.minBitrate = 1;
                }

                dbg(1, "set min bitrate to: %d",
                    info.minBitrate);
            }
            else if (strcmp(long_opts[opt_ndx].name,
                      "max-quant-lvl") == 0)
            {
                info.maxQuantLevel = atoi(optarg);
                if (info.maxQuantLevel > atoi(MAX_QUANT_LVL))
                {
                    g_print("Maximum quant-lvl is "
                        MAX_QUANT_LVL
                        ".\n");
                    info.maxQuantLevel =
                        atoi(MAX_QUANT_LVL);
                }
                else if (info.maxQuantLevel <
                       atoi(MIN_QUANT_LVL))
                {
                    g_print("Minimum quant-lvl is "
                        MIN_QUANT_LVL
                        ".\n");
                    info.maxQuantLevel =
                        atoi(MIN_QUANT_LVL);
                }
                dbg(1, "set max quant to: %d",
                    info.maxQuantLevel);
            }
            else if (strcmp(long_opts[opt_ndx].name,
                          "command-pipe") == 0)
            {
                    info.commandPipe = optarg;
                    dbg(1, "set command pipe to: %s",
                        info.commandPipe);
            }
            else if (strcmp(long_opts[opt_ndx].name,
                          "status-pipe") == 0)
            {
                    info.statusPipe = optarg;
                    dbg(1, "set status pipe to: %s",
                        info.statusPipe);
            }
            else if (strcmp(long_opts[opt_ndx].name,
                         "enable-shared-pipeline") == 0)
            {
                    info.enableSharedPipeline = TRUE;
                    dbg(1, "set enable-shared-pipeline to: %d", info.enableVariableMode);
                    break;
            }
            else if (strcmp(long_opts[opt_ndx].name,
                         "enable-no-suspend") == 0)
            {
                    info.enableNoSuspend = TRUE;
                    dbg(1, "set enable-no-suspend to: %d", info.enableVariableMode);
                    break;
            }
            else if (strcmp(long_opts[opt_ndx].name,
                         "client-port-min") == 0)
            {
                    info.rtspPortMin = atoi(optarg);
                    dbg(1, "set client-port-min to: %d", info.rtspPortMin);
                    break;
            }
            else if (strcmp(long_opts[opt_ndx].name,
                         "client-port-max") == 0)
            {
                    info.rtspPortMax = atoi(optarg);
                    dbg(1, "set client-port-max to: %d", info.rtspPortMax);
                    break;

            }
            else
            {
                puts(usage);
                return -ECODE_ARGS;
            }
            break;
        case 'h': // Help
        case '?':
            puts(usage);
            return ECODE_OKAY;
        case 'v': // Version
            puts("Program Version: " VERSION);
            return ECODE_OKAY;
        case 'd':
            debugLevel = atoi(optarg);
            dbg(1, "set debug level to: %d", debugLevel);
            break;
        case 'm': // Mount Point
            mountPoint = optarg;
            dbg(1, "set mount point to: %s", mountPoint);
            break;
        case 'p': // Port
            port = optarg;
            dbg(1, "set port to: %s", port);
            break;
        case 'u': // User Pipeline
            info.userpipeline = optarg;
            dbg(1, "set user pipeline to: %s", info.userpipeline);
            break;
        case 's': // Video source element
            sourceElement = optarg;
            dbg(1, "set source element to: %s", sourceElement);
            break;
        case 'i': // Video in parameter
            info.videoInDevice = optarg;
            dbg(1, "set video in to: %s", info.videoInDevice);
            break;
        case 'e': // enable-variable-mode
            info.enableVariableMode = TRUE;
            dbg(1, "set enable-variable-mode to: %d", info.enableVariableMode);
            break;

        case 'b': // Max Bitrate
            info.maxBitrate = atoi(optarg);
            if (info.maxBitrate > atoi(MAX_BR))
            {
                g_print("Maximum bitrate is " MAX_BR ".\n");
                info.maxBitrate = atoi(MAX_BR);
            }
            else if (info.maxBitrate < atoi(MIN_BR))
            {
                g_print("Minimum bitrate is " MIN_BR ".\n");
                info.maxBitrate = atoi(MIN_BR);
            }

            dbg(1, "set max bitrate to: %d", info.maxBitrate);
            break;
        case 'l':
            info.minQuantLevel = atoi(optarg);
            if (info.minQuantLevel > atoi(MAX_QUANT_LVL))
            {
                g_print("Maximum quant-lvl is " MAX_QUANT_LVL
                    ".\n");
                info.minQuantLevel = atoi(MAX_QUANT_LVL);
            }
            else if (info.minQuantLevel < atoi(MIN_QUANT_LVL))
            {
                g_print("Minimum quant-lvl is " MIN_QUANT_LVL
                    ".\n");
                info.minQuantLevel = atoi(MIN_QUANT_LVL);
            }

            info.currentQuantLevel = info.minQuantLevel;
            dbg(1, "set min quant lvl to: %d",
                info.minQuantLevel);
            break;
        case 'c': // config-interval
            info.configInterval = atoi(optarg);
            dbg(1, "set rtsp config interval to: %d",
                info.configInterval);
            break;
        case 'a': // idr frame interval
            info.idr = atoi(optarg);
            dbg(1, "set idr interval to: %d", info.idr);
            break;
        case 'r': // how often to display messages at
            info.periodicStatusMessageRate = atoi(optarg);
            dbg(1, "set msg rate to: %d", info.periodicStatusMessageRate);
            break;
        default: // Default - bad arg
            puts(usage);
            return -ECODE_ARGS;
        }
    }


    if (info.maxBitrate > info.capBitrate)
    {
        g_printerr("Max bitrate must be <= cap bitrate, setting max bit rate to %d.\n",info.capBitrate );
        info.maxBitrate = info.capBitrate;
    }

    info.currentBitrate = info.capBitrate;
    // Validate inputs
    if (info.maxQuantLevel < info.minQuantLevel)
    {
        g_printerr("Max Quant level must be"
               "greater than Min Quant level\n");
        return -ECODE_ARGS;
    }

    if (info.maxBitrate>0 && info.maxBitrate <= info.minBitrate)
    {
        g_printerr("Max bitrate must be greater than min bitrate\n");
        return -ECODE_ARGS;
    }

    if (info.steps < 1)
    {
        // Because we subtract 1 off of user input of steps,
        // we must account for it here when reporting to user
        g_printerr("Steps must be 2 or greater\n");
        return -ECODE_ARGS;
    }

    if (info.rtspPortMin > 0 || info.rtspPortMax > 0)
    {
        if (info.rtspPortMin <=0)
        {
            g_printerr("Rtsp Port min not valid. (min:%d)\n", info.rtspPortMin);
            return -ECODE_RTSP;
        }
        if (info.rtspPortMax <= 0)
        {
            g_printerr("Rtsp Port max not valid. (max:%d)\n", info.rtspPortMax);
            return -ECODE_RTSP;
        }
        if (info.rtspPortMax <= info.rtspPortMin)
        {
            g_printerr("Rtsp port max must be greater than Rtsp port min . (min:%d, max:%d)\n", info.rtspPortMin, info.rtspPortMax);
            return -ECODE_RTSP;
        }
    }

    // Configure RTSP
    info.server = gst_rtsp_server_new();
    if (!info.server)
    {
        g_printerr("Could not create RTSP server\n");
        return -ECODE_RTSP;
    }
    g_object_set(info.server, "service", port, NULL);

    // Map URI mount points to media factories
    info.mounts = gst_rtsp_server_get_mount_points(info.server);
    info.factory = gst_rtsp_media_factory_new();
    if (!info.factory)
    {
        g_printerr("Could not create RTSP server\n");
        return -ECODE_RTSP;
    }

    if (info.enableNoSuspend)
    {
      gst_rtsp_media_factory_set_suspend_mode (info.factory, GST_RTSP_SUSPEND_MODE_NONE);
    }
    if (info.rtspPortMin > 0 || info.rtspPortMax > 0)
    {
        info.rtspAddressPool = gst_rtsp_address_pool_new();

        if (info.rtspAddressPool != NULL)
        {
            const unsigned char RTSP_TTL = 0;
            gboolean rangeAded = gst_rtsp_address_pool_add_range(info.rtspAddressPool,
                    GST_RTSP_ADDRESS_POOL_ANY_IPV4, GST_RTSP_ADDRESS_POOL_ANY_IPV4,
                    info.rtspPortMin, info.rtspPortMax, RTSP_TTL);

            if (rangeAded == TRUE)
            {
                gst_rtsp_media_factory_set_address_pool(info.factory, info.rtspAddressPool);
            }
            else
            {
                g_printerr("Failed to set RTSP media factory address pool");
                return -ECODE_RTSP;
            }
        }
        else
        {
            g_printerr( "Failed to create RTSP address pool");

            return -ECODE_RTSP;
        }
    }

if (info.enableSharedPipeline)
    {
        // Share single pipeline with all clients
        gst_rtsp_media_factory_set_shared(info.factory, TRUE);
    }
    // Source Pipeline
    if (info.userpipeline)
    {
        snprintf(launch, LAUNCH_MAX, "( %s )", info.userpipeline);
    }
    else
    {
        snprintf(launch, LAUNCH_MAX, "%s name=source0 ! "
          STATIC_SINK_PIPELINE,
          sourceElement
          );
    }

    g_print("Pipeline set to: %s...\n", launch);
    gst_rtsp_media_factory_set_launch(info.factory, launch);

    // Connect pipeline to the mount point (URI)
    gst_rtsp_mount_points_add_factory(info.mounts, mountPoint,
                      info.factory);


    // Create GLIB MainContext
    info.mainLoop = g_main_loop_new(NULL, FALSE);



    // Attach server to default maincontext
    ret = gst_rtsp_server_attach(info.server, NULL);
    if (ret == FALSE)
    {
        g_printerr("Unable to attach RTSP server\n");
        return -ECODE_RTSP;
    }

    // Configure Callbacks and signals

    // setup command pipe if specified
    if (info.commandPipe!=NULL)
    {
        //do not remove old ones if they exist, expect the creating process to do this
        //so it can open the pipe.

        mkfifo(info.commandPipe, 0666);
        info.commandPipeFd = open(info.commandPipe, O_RDONLY | O_NONBLOCK );

        if (info.commandPipeFd<0)
        {
            g_printerr("Could not open command pipe [%s], exiting now.\n", info.commandPipe);
            exit(1);
        }
        if (info.statusPipe!=NULL)
        {
            dbg(4, "Creating status pipe [%s]", info.statusPipe);

            mkfifo(info.statusPipe, 0666);
            //Will be opened later, you don't want to open this now,
            //as it blocks until someone reads from it at least once.
        }
        g_timeout_add(100, (GSourceFunc)reader, &info);
    }

    // Create new client handler (Called on new client connect)

    dbg(2, "Creating 'client-connected' signal handler");
    g_signal_connect(info.server, "client-connected",
             G_CALLBACK(newClientHandler), &info);

    // Run GBLIB main loop until it returns
    g_print("Stream ready at rtsp://" DEFAULT_HOST ":%s%s\n",
        port, mountPoint);
    g_main_loop_run(info.mainLoop);

    // Cleanup
    g_main_loop_unref(info.mainLoop);
    g_object_unref(info.factory);
    g_object_unref(info.media);
    g_object_unref(info.mounts);

    return ECODE_OKAY;
}


