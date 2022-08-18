/*
 * obs-vaapi. OBS Studio plugin.
 * Copyright (C) 2022 Florian Zwoch <fzwoch@gmail.com>
 *
 * This file is part of obs-vaapi.
 *
 * obs-vaapi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * obs-vaapi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with obs-vaapi. If not, see <http://www.gnu.org/licenses/>.
 */

#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/app.h>

#define ENCODER_TYPE_DATA_H264 "VAAPI H.264"
#define ENCODER_TYPE_DATA_H265 "VAAPI H.265"

OBS_DECLARE_MODULE()

typedef struct {
	obs_encoder_t *encoder;
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	GstSample *sample;
	GstMapInfo info;
	void *codec_data;
	size_t codec_size;
} obs_vaapi_t;

static gboolean bus_callback(GstBus *bus, GstMessage *message,
			     gpointer user_data)
{
	GError *err = NULL;

	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_WARNING:
		gst_message_parse_warning(message, &err, NULL);
		blog(LOG_WARNING, "[obs-vaapi] %s", err->message);
		g_error_free(err);
		break;
	case GST_MESSAGE_ERROR:
		gst_message_parse_error(message, &err, NULL);
		blog(LOG_ERROR, "[obs-vaapi] %s", err->message);
		g_error_free(err);
		break;
	default:
		break;
	}

	return TRUE;
}

static void enough_data()
{
	blog(LOG_WARNING, "[obs-vaapi] encoder overload");
}

static const char *get_name(void *type_data)
{
	return (const char *)type_data;
}

static void *create(obs_data_t *settings, obs_encoder_t *encoder)
{
	obs_vaapi_t *vaapi = bzalloc(sizeof(obs_vaapi_t));

	vaapi->encoder = encoder;

	obs_encoder_set_preferred_video_format(encoder, VIDEO_FORMAT_NV12);

	struct obs_video_info video_info;
	obs_get_video_info(&video_info);

	vaapi->pipe = gst_pipeline_new(NULL);
	vaapi->appsrc = gst_element_factory_make("appsrc", NULL);
	vaapi->appsink = gst_element_factory_make("appsink", NULL);

	GstCaps *caps = gst_caps_new_simple(
		"video/x-raw", "format", G_TYPE_STRING, "NV12", "framerate",
		GST_TYPE_FRACTION, video_info.fps_num, video_info.fps_den,
		"width", G_TYPE_INT, obs_encoder_get_width(encoder), "height",
		G_TYPE_INT, obs_encoder_get_height(encoder), "interlace-mode",
		G_TYPE_STRING, "progressive", NULL);

	g_object_set(vaapi->appsrc, "caps", caps, NULL);
	gst_caps_unref(caps);

	GstElement *vaapiencoder = NULL;
	GstElement *parser = NULL;

	g_object_set(vaapi->appsrc, "max-buffers", 1, "max-bytes", 0,
		     "max-time", 0, NULL);
	g_object_set(vaapi->appsink, "sync", FALSE, NULL);

	g_signal_connect(vaapi->appsrc, "enough-data", G_CALLBACK(enough_data),
			 NULL);

	if (g_strcmp0(obs_encoder_get_codec(encoder), "h264") == 0) {
		vaapiencoder = gst_element_factory_make("vaapih264enc", NULL);
		parser = gst_element_factory_make("h264parse", NULL);

		caps = gst_caps_new_simple("video/x-h264", "stream-format",
					   G_TYPE_STRING, "byte-stream",
					   "alignment", G_TYPE_STRING, "au",
					   NULL);

		g_object_set(vaapi->appsink, "caps", caps, NULL);
		gst_caps_unref(caps);
	} else if (g_strcmp0(obs_encoder_get_codec(encoder), "hevc") == 0) {
		vaapiencoder = gst_element_factory_make("vaapih265enc", NULL);
		parser = gst_element_factory_make("h265parse", NULL);

		caps = gst_caps_new_simple("video/x-h265", "stream-format",
					   G_TYPE_STRING, "byte-stream",
					   "alignment", G_TYPE_STRING, "au",
					   NULL);

		g_object_set(vaapi->appsink, "caps", caps, NULL);
		gst_caps_unref(caps);
	} else {
		blog(LOG_ERROR, "[obs-vaapi] unhandled codec: %s",
		     obs_encoder_get_codec(encoder));
		bfree(vaapi);
		return NULL;
	}

	gst_bin_add_many(GST_BIN(vaapi->pipe), vaapi->appsrc, vaapiencoder,
			 parser, vaapi->appsink, NULL);
	gst_element_link_many(vaapi->appsrc, vaapiencoder, parser,
			      vaapi->appsink, NULL);

	obs_properties_t *properties = obs_encoder_properties(encoder);
	for (obs_property_t *property = obs_properties_first(properties);
	     property; obs_property_next(&property)) {
		switch (obs_property_get_type(property)) {
		case OBS_PROPERTY_TEXT:
			g_object_set(vaapiencoder, obs_property_name(property),
				     obs_data_get_string(
					     settings,
					     obs_property_name(property)),
				     NULL);
			break;
		case OBS_PROPERTY_INT:
			g_object_set(
				vaapiencoder, obs_property_name(property),
				obs_data_get_int(settings,
						 obs_property_name(property)),
				NULL);
			break;
		case OBS_PROPERTY_BOOL:
			g_object_set(
				vaapiencoder, obs_property_name(property),
				obs_data_get_bool(settings,
						  obs_property_name(property)),
				NULL);
			break;
		case OBS_PROPERTY_FLOAT:
			g_object_set(vaapiencoder, obs_property_name(property),
				     obs_data_get_double(
					     settings,
					     obs_property_name(property)),
				     NULL);
			break;
		case OBS_PROPERTY_LIST:
			gst_util_set_object_arg(
				G_OBJECT(vaapiencoder),
				obs_property_name(property),
				obs_data_get_string(
					settings, obs_property_name(property)));
			break;
		default:
			blog(LOG_WARNING, "[obs-vaapi] unhandled property: %s",
			     obs_property_name(property));
			break;
		}
	}
	obs_properties_destroy(properties);

	GstBus *bus = gst_element_get_bus(vaapi->pipe);
	gst_bus_add_watch(bus, bus_callback, NULL);
	gst_object_unref(bus);

	gst_element_set_state(vaapi->pipe, GST_STATE_PLAYING);

	return vaapi;
}

static void destroy(void *data)
{
	obs_vaapi_t *vaapi = (obs_vaapi_t *)data;

	if (vaapi->pipe) {
		gst_element_set_state(vaapi->pipe, GST_STATE_NULL);

		GstBus *bus = gst_element_get_bus(vaapi->pipe);
		gst_bus_remove_watch(bus);
		gst_object_unref(bus);

		gst_object_unref(vaapi->pipe);
	}

	if (vaapi->sample) {
		GstBuffer *buffer = gst_sample_get_buffer(vaapi->sample);
		gst_buffer_unmap(buffer, &vaapi->info);
		gst_sample_unref(vaapi->sample);
	}

	bfree(vaapi->codec_data);
	bfree(vaapi);
}

static bool encode(void *data, struct encoder_frame *frame,
		   struct encoder_packet *packet, bool *received_packet)
{
	obs_vaapi_t *vaapi = (obs_vaapi_t *)data;

	if (vaapi->sample) {
		GstBuffer *buffer = gst_sample_get_buffer(vaapi->sample);
		gst_buffer_unmap(buffer, &vaapi->info);
		gst_sample_unref(vaapi->sample);
		vaapi->sample = NULL;
	}

	GstBuffer *buffer = gst_buffer_new_wrapped_full(
		0, frame->data[0],
		obs_encoder_get_width(vaapi->encoder) *
			obs_encoder_get_width(vaapi->encoder) * 3 / 2,
		0,
		obs_encoder_get_width(vaapi->encoder) *
			obs_encoder_get_width(vaapi->encoder) * 3 / 2,
		NULL, NULL);

	GST_BUFFER_PTS(buffer) =
		frame->pts *
		(GST_SECOND / (packet->timebase_den / packet->timebase_num));

	gst_app_src_push_buffer(GST_APP_SRC(vaapi->appsrc), buffer);

	vaapi->sample =
		gst_app_sink_try_pull_sample(GST_APP_SINK(vaapi->appsink), 0);
	if (vaapi->sample == NULL) {
		return true;
	}

	*received_packet = true;

	buffer = gst_sample_get_buffer(vaapi->sample);

	gst_buffer_map(buffer, &vaapi->info, GST_MAP_READ);

	if (vaapi->codec_data == NULL) {
		vaapi->codec_data = bmemdup(vaapi->info.data, vaapi->info.size);
		vaapi->codec_size = vaapi->info.size;
	}

	packet->data = vaapi->info.data;
	packet->size = vaapi->info.size;

	packet->pts = GST_BUFFER_PTS(buffer);
	packet->dts = GST_BUFFER_DTS(buffer);

	packet->pts /=
		GST_SECOND / (packet->timebase_den / packet->timebase_num);
	packet->dts /=
		GST_SECOND / (packet->timebase_den / packet->timebase_num);

	packet->type = OBS_ENCODER_VIDEO;

	packet->keyframe =
		!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

	return true;
}

static void get_defaults2(obs_data_t *settings, void *type_data)
{
	GstElement *encoder;

	if (g_strcmp0(type_data, ENCODER_TYPE_DATA_H264) == 0) {
		encoder = gst_element_factory_make("vaapih264enc", NULL);
	} else if (g_strcmp0(type_data, ENCODER_TYPE_DATA_H265) == 0) {
		encoder = gst_element_factory_make("vaapih265enc", NULL);
	} else {
		blog(LOG_ERROR, "[obs-vaapi] unhandled encoder type");
		return;
	}

	if (encoder == NULL) {
		blog(LOG_ERROR, "[obs-vaapi] could not load encoder: %s",
		     (const char *)type_data);
		return;
	}

	guint num_properties;
	GParamSpec **property_specs = g_object_class_list_properties(
		G_OBJECT_GET_CLASS(encoder), &num_properties);

	for (guint i = 0; i < num_properties; i++) {
		GParamSpec *param = property_specs[i];

		if (param->owner_type == G_TYPE_OBJECT ||
		    param->owner_type == GST_TYPE_OBJECT ||
		    param->owner_type == GST_TYPE_PAD) {
			continue;
		}

		GValue value = {
			0,
		};
		g_value_init(&value, param->value_type);

		g_object_get_property(G_OBJECT(encoder), param->name, &value);

		gchar *str;
		gint uint64;
		gint int64;
		gint uint32;
		gint int32;
		gboolean boolean;
		gfloat float32;
		gdouble float64;

		switch (G_VALUE_TYPE(&value)) {
		case G_TYPE_STRING:
			g_object_get(encoder, param->name, &str, NULL);
			obs_data_set_default_string(settings, param->name, str);
			break;
		case G_TYPE_UINT64:
			g_object_get(encoder, param->name, &uint64, NULL);
			obs_data_set_default_int(settings, param->name, uint64);
			break;
		case G_TYPE_INT64:
			g_object_get(encoder, param->name, &int64, NULL);
			obs_data_set_default_int(settings, param->name, int64);
			break;
		case G_TYPE_UINT:
			g_object_get(encoder, param->name, &uint32, NULL);
			obs_data_set_default_int(settings, param->name, uint32);
			break;
		case G_TYPE_INT:
			g_object_get(encoder, param->name, &int32, NULL);
			obs_data_set_default_int(settings, param->name, int32);
			break;
		case G_TYPE_BOOLEAN:
			g_object_get(encoder, param->name, &boolean, NULL);
			obs_data_set_default_bool(settings, param->name,
						  boolean);
			break;
		case G_TYPE_FLOAT:
			g_object_get(encoder, param->name, &float32, NULL);
			obs_data_set_default_double(settings, param->name,
						    float32);
			break;
		case G_TYPE_DOUBLE:
			g_object_get(encoder, param->name, &float64, NULL);
			obs_data_set_default_double(settings, param->name,
						    float64);
			break;
		default:
			if (G_IS_PARAM_SPEC_ENUM(param)) {
				GEnumValue *values =
					G_ENUM_CLASS(g_type_class_ref(
							     param->value_type))
						->values;
				gint enum_value = g_value_get_enum(&value);
				for (int j = 0; values[j].value_name; j++) {
					if (values[j].value == enum_value) {
						obs_data_set_default_string(
							settings, param->name,
							values[j].value_name);
						break;
					}
				}
			} else if (GST_IS_PARAM_SPEC_ARRAY_LIST(param)) {
				// not implemented
			} else {
				blog(LOG_WARNING,
				     "[obs-vaapi] unhandled property: %s",
				     param->name);
			}
			break;
		}
	}

	g_free(property_specs);
	gst_object_unref(encoder);
}

static obs_properties_t *get_properties2(void *data, void *type_data)
{
	GstElement *encoder;

	if (g_strcmp0(type_data, ENCODER_TYPE_DATA_H264) == 0) {
		encoder = gst_element_factory_make("vaapih264enc", NULL);
	} else if (g_strcmp0(type_data, ENCODER_TYPE_DATA_H265) == 0) {
		encoder = gst_element_factory_make("vaapih265enc", NULL);
	} else {
		blog(LOG_ERROR, "[obs-vaapi] unhandled encoder type");
		return NULL;
	}

	if (encoder == NULL) {
		blog(LOG_ERROR, "[obs-vaapi] could not load encoder: %s",
		     (const char *)type_data);
		return NULL;
	}

	obs_properties_t *properties = obs_properties_create();
	guint num_properties;
	GParamSpec **property_specs = g_object_class_list_properties(
		G_OBJECT_GET_CLASS(encoder), &num_properties);

	for (guint i = 0; i < num_properties; i++) {
		obs_property_t *property;
		GParamSpec *param = property_specs[i];

		if (param->owner_type == G_TYPE_OBJECT ||
		    param->owner_type == GST_TYPE_OBJECT ||
		    param->owner_type == GST_TYPE_PAD) {
			continue;
		}

		GValue value = {
			0,
		};
		g_value_init(&value, param->value_type);

		g_object_get_property(G_OBJECT(encoder), param->name, &value);

		switch (G_VALUE_TYPE(&value)) {
		case G_TYPE_STRING:
			property = obs_properties_add_text(properties,
							   param->name,
							   param->name,
							   OBS_TEXT_DEFAULT);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_UINT64:
			property = obs_properties_add_int(
				properties, param->name, param->name,
				G_PARAM_SPEC_UINT64(param)->minimum,
				MIN(G_PARAM_SPEC_UINT64(param)->maximum,
				    G_MAXINT32),
				1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_INT64:
			property = obs_properties_add_int(
				properties, param->name, param->name,
				G_PARAM_SPEC_INT64(param)->minimum,
				MIN(G_PARAM_SPEC_INT64(param)->maximum,
				    G_MAXINT32),
				1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_UINT:
			property = obs_properties_add_int(
				properties, param->name, param->name,
				G_PARAM_SPEC_UINT(param)->minimum,
				MIN(G_PARAM_SPEC_UINT(param)->maximum,
				    G_MAXINT32),
				1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_INT:
			property = obs_properties_add_int(
				properties, param->name, param->name,
				G_PARAM_SPEC_INT(param)->minimum,
				G_PARAM_SPEC_INT(param)->maximum, 1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_BOOLEAN:
			property = obs_properties_add_bool(
				properties, param->name, param->name);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_FLOAT:
			property = obs_properties_add_float(
				properties, param->name, param->name,
				G_PARAM_SPEC_FLOAT(param)->minimum,
				G_PARAM_SPEC_FLOAT(param)->maximum, 0.1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_DOUBLE:
			property = obs_properties_add_float(
				properties, param->name, param->name,
				G_PARAM_SPEC_DOUBLE(param)->minimum,
				G_PARAM_SPEC_DOUBLE(param)->maximum, 0.1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		default:
			if (G_IS_PARAM_SPEC_ENUM(param)) {
				property = obs_properties_add_list(
					properties, param->name, param->name,
					OBS_COMBO_TYPE_LIST,
					OBS_COMBO_FORMAT_STRING);
				GEnumValue *values =
					G_ENUM_CLASS(g_type_class_ref(
							     param->value_type))
						->values;
				for (int j = 0; values[j].value_name; j++) {
					obs_property_list_add_string(
						property, values[j].value_name,
						values[j].value_nick);
				}
				obs_property_set_long_description(
					property,
					g_param_spec_get_blurb(param));
			} else if (GST_IS_PARAM_SPEC_ARRAY_LIST(param)) {
				// not implemented
			} else {
				blog(LOG_WARNING,
				     "[obs-vaapi] unhandled property: %s",
				     param->name);
			}
			break;
		}
	}

	g_free(property_specs);
	gst_object_unref(encoder);

	return properties;
}

static bool get_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	obs_vaapi_t *vaapi = (obs_vaapi_t *)data;

	if (vaapi->codec_data == NULL) {
		return false;
	}

	// We return the first AU. It includes required data. But it also
	// includes the first frame. We hope that downstream has a parser to
	// pick the relevant data it wants.
	*extra_data = vaapi->codec_data;
	*size = vaapi->codec_size;

	return true;
}

MODULE_EXPORT bool obs_module_load(void)
{
	gst_init(NULL, NULL);

	struct obs_encoder_info vaapi = {
		.id = "obs-vaapi-h264",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",
		.get_name = get_name,
		.create = create,
		.destroy = destroy,
		.get_defaults2 = get_defaults2,
		.get_properties2 = get_properties2,
		.encode = encode,
		.get_extra_data = get_extra_data,
		.type_data = ENCODER_TYPE_DATA_H264,
	};

	obs_register_encoder(&vaapi);

	vaapi.id = "obs-vaapi-h265";
	vaapi.codec = "hevc";
	vaapi.type_data = ENCODER_TYPE_DATA_H265;

	obs_register_encoder(&vaapi);

	return true;
}
