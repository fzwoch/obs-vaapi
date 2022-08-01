#include <obs-module.h>
#include <gst/gst.h>
#include <gst/app/app.h>

OBS_DECLARE_MODULE()

typedef struct {
	obs_encoder_t *encoder;
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
} obs_vaapi_t;

static const char *get_name(void *type_data)
{
	return "VAAPI H.264";
}

static void *create(obs_data_t *settings, obs_encoder_t *encoder)
{
	obs_vaapi_t *vaapi = bzalloc(sizeof(obs_vaapi_t));

	vaapi->encoder = encoder;

	GError *err = NULL;
	vaapi->pipe = gst_parse_launch(
		"appsrc name=appsrc ! vaapih264enc name=encoder ! h264parse ! video/x-h264, stream-format=byte-stream, alignment=au ! appsink sync=false name=appsink",
		&err);
	if (err) {
		blog(LOG_ERROR, "[obs-vaapi] %s", err->message);

		g_error_free(err);
		bfree(vaapi);

		return NULL;
	}

	GstElement *element =
		gst_bin_get_by_name(GST_BIN(vaapi->pipe), "encoder");

	for (obs_property_t *property =
		     obs_properties_first(obs_encoder_properties(encoder));
	     property; obs_property_next(&property)) {

		switch (obs_property_get_type(property)) {
		case OBS_PROPERTY_TEXT:
			g_object_set(element, obs_property_name(property),
				     obs_data_get_string(
					     settings,
					     obs_property_name(property)),
				     NULL);
			break;
		case OBS_PROPERTY_INT:
			g_object_set(
				element, obs_property_name(property),
				obs_data_get_int(settings,
						 obs_property_name(property)),
				NULL);
			break;
		case OBS_PROPERTY_BOOL:
			g_object_set(
				element, obs_property_name(property),
				obs_data_get_bool(settings,
						  obs_property_name(property)),
				NULL);
			break;
		case OBS_PROPERTY_FLOAT:
			g_object_set(element, obs_property_name(property),
				     obs_data_get_double(
					     settings,
					     obs_property_name(property)),
				     NULL);
			break;
		default:
			blog(LOG_WARNING, "[obs-vaapi] unhandled property: %s",
			     obs_property_name(property));
			break;
		}
	}

	gst_object_unref(element);

	vaapi->appsrc = gst_bin_get_by_name(GST_BIN(vaapi->pipe), "appsrc");
	vaapi->appsink = gst_bin_get_by_name(GST_BIN(vaapi->pipe), "appsink");

	GstCaps *caps = gst_caps_new_simple(
		"video/x-raw", "format", G_TYPE_STRING, "NV12", "framerate",
		GST_TYPE_FRACTION, 0, 1, "width", G_TYPE_INT,
		obs_encoder_get_width(encoder), "height", G_TYPE_INT,
		obs_encoder_get_height(encoder), "interlace-mode",
		G_TYPE_STRING, "progressive", NULL);

	g_object_set(vaapi->appsrc, "caps", caps, NULL);

	gst_caps_unref(caps);
	gst_element_set_state(vaapi->pipe, GST_STATE_PLAYING);

	return vaapi;
}

static void destroy(void *data)
{
	obs_vaapi_t *vaapi = (obs_vaapi_t *)data;

	if (vaapi->pipe) {
		gst_element_set_state(vaapi->pipe, GST_STATE_NULL);
		gst_object_unref(vaapi->appsink);
		gst_object_unref(vaapi->appsrc);
		gst_object_unref(vaapi->pipe);
	}

	bfree(vaapi);
}

static bool encode(void *data, struct encoder_frame *frame,
		   struct encoder_packet *packet, bool *received_packet)
{
	obs_vaapi_t *vaapi = (obs_vaapi_t *)data;

	GstBuffer *buffer = gst_buffer_new_wrapped_full(
		0, frame->data[0],
		obs_encoder_get_width(vaapi->encoder) *
			obs_encoder_get_width(vaapi->encoder) * 3 / 2,
		0,
		obs_encoder_get_width(vaapi->encoder) *
			obs_encoder_get_width(vaapi->encoder) * 3 / 2,
		NULL, NULL);

	//	GST_BUFFER_PTS(buffer) =
	//		frame->pts *
	//		(GST_SECOND / (data->ovi.fps_num / data->ovi.fps_den));

	gst_app_src_push_buffer(GST_APP_SRC(vaapi->appsrc), buffer);

	GstSample *sample =
		gst_app_sink_try_pull_sample(GST_APP_SINK(vaapi->appsink), 0);
	if (sample == NULL) {
		return true;
	}

	*received_packet = true;

	buffer = gst_sample_get_buffer(sample);

	GstMapInfo info;

	gst_buffer_map(buffer, &info, GST_MAP_READ);

	packet->data = info.data; //FIXME
	packet->size = info.size;

	gst_buffer_unmap(buffer, &info);

	packet->pts = GST_BUFFER_PTS(buffer);
	packet->dts = GST_BUFFER_DTS(buffer);

	packet->pts /=
		GST_SECOND / (packet->timebase_den / packet->timebase_num);
	packet->dts /=
		GST_SECOND / (packet->timebase_den / packet->timebase_num);

	packet->type = OBS_ENCODER_VIDEO;

	packet->keyframe =
		!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

	gst_sample_unref(sample);

	return true;
}

/*

warning: [obs-vaapi] unhandled property: rate-control
warning: [obs-vaapi] unhandled property: tune
warning: [obs-vaapi] unhandled property: view-ids
warning: [obs-vaapi] unhandled property: compliance-mode
warning: [obs-vaapi] unhandled property: mbbrc
warning: [obs-vaapi] unhandled property: prediction-type


*/

static void get_defaults(obs_data_t *settings)
{
	GstElementFactory *factory = gst_element_factory_find("vaapih264enc");
	if (factory == NULL) {
		blog(LOG_ERROR, "[obs-vaapi] vaapih264enc not found");
		return;
	}

	GstElement *encoder = gst_element_factory_create(factory, NULL);

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

		switch (G_VALUE_TYPE(&value)) {
		case G_TYPE_STRING:
			gchar *str;
			g_object_get(encoder, param->name, &str, NULL);
			obs_data_set_default_string(settings, param->name, str);
			break;
		case G_TYPE_UINT64:
		case G_TYPE_INT64:
		case G_TYPE_UINT:
		case G_TYPE_INT:
			gint integer;
			g_object_get(encoder, param->name, &integer, NULL);
			obs_data_set_default_int(settings, param->name,
						 integer);
			break;
		case G_TYPE_BOOLEAN:
			gboolean boolean;
			g_object_get(encoder, param->name, &boolean, NULL);
			obs_data_set_default_bool(settings, param->name,
						  boolean);
			break;
		case G_TYPE_FLOAT:
		case G_TYPE_DOUBLE:
			gdouble fp;
			g_object_get(encoder, param->name, &fp, NULL);
			obs_data_set_default_double(settings, param->name, fp);
			break;
		default:
			blog(LOG_WARNING, "[obs-vaapi] unhandled property: %s",
			     param->name);
		}
	}

	g_free(property_specs);
	gst_object_unref(encoder);
	gst_object_unref(factory);
}

static obs_properties_t *get_properties(void *data)
{
	obs_properties_t *properties = obs_properties_create();

	GstElementFactory *factory = gst_element_factory_find("vaapih264enc");
	if (factory == NULL) {
		blog(LOG_ERROR, "[obs-vaapi] vaapih264enc not found");
		return NULL;
	}

	GstElement *encoder = gst_element_factory_create(factory, NULL);

	obs_property_t *property;
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
				G_PARAM_SPEC_UINT64(param)->maximum, 1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_INT64:
			property = obs_properties_add_int(
				properties, param->name, param->name,
				G_PARAM_SPEC_INT64(param)->minimum,
				G_PARAM_SPEC_INT64(param)->maximum, 1);
			obs_property_set_long_description(
				property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_UINT:
			property = obs_properties_add_int(
				properties, param->name, param->name,
				G_PARAM_SPEC_UINT(param)->minimum,
				G_PARAM_SPEC_UINT(param)->maximum, 1);
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
			blog(LOG_WARNING, "[obs-vaapi] unhandled property: %s",
			     param->name);
		}
	}

	g_free(property_specs);
	gst_object_unref(encoder);
	gst_object_unref(factory);

	return properties;
}

bool obs_module_load(void)
{
	gst_init(NULL, NULL);

	struct obs_encoder_info vaapi = {
		.id = "obs-vaapi-h264",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",
		.get_name = get_name,
		.create = create,
		.destroy = destroy,
		.get_defaults = get_defaults,
		.get_properties = get_properties,
		.encode = encode,
		/*     .update         = my_encoder_update,
        .get_extra_data = my_encoder_extra_data,
        .get_sei_data   = my_encoder_sei,
        .get_video_info = my_encoder_video_info
        */
	};

	obs_register_encoder(&vaapi);

	return true;
}
