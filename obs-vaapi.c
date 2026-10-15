/*
 * obs-vaapi. OBS Studio plugin.
 * Copyright (C) 2022-2023 Florian Zwoch <fzwoch@gmail.com>
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

#define _GNU_SOURCE

#define GST_USE_UNSTABLE_API

#include <gst/app/app.h>
#include <gst/gst.h>
#include <gst/va/gstva.h>
#include <gst/video/video.h>
#include <obs/obs-module.h>
#include <pci/pci.h>

#include <drm/drm_fourcc.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drmcommon.h>

OBS_DECLARE_MODULE()

#define TEXTURE_PREFIX "obs-va-tex-"

static GHashTable *hash_table;

typedef struct {
	obs_encoder_t *encoder;
	GstElement *pipe;
	GstElement *appsrc;
	GstElement *appsink;
	GstSample *sample;
	GstMapInfo info;
	GMutex mutex;
	GCond cond;
	bool consumed;
	void *codec_data;
	size_t codec_size;
	struct obs_video_info video_info;
	GstVaDisplay *va_display;
	GstAllocator *va_allocator;
	GstVideoInfoDmaDrm drm_info;
} obs_vaapi_t;

static enum gs_color_format drm_to_gs_color_format(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_R8:
		return GS_R8;
	case DRM_FORMAT_R16:
		return GS_R16;
	case DRM_FORMAT_GR88:
		return GS_R8G8;
	case DRM_FORMAT_GR1616:
		return GS_RG16;
	default:
		blog(LOG_ERROR, "[obs-vaapi] unsupported drm format: %d", format);
		return GS_UNKNOWN;
	}
}

static GstVideoFormat map_video_format(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
		return GST_VIDEO_FORMAT_I420;
	case VIDEO_FORMAT_NV12:
		return GST_VIDEO_FORMAT_NV12;
	case VIDEO_FORMAT_I444:
		return GST_VIDEO_FORMAT_Y444;
	case VIDEO_FORMAT_BGRA:
		return GST_VIDEO_FORMAT_BGRx;
	case VIDEO_FORMAT_P010:
		return GST_VIDEO_FORMAT_P010_10LE;
	case VIDEO_FORMAT_I010:
		return GST_VIDEO_FORMAT_I420_10LE;
	default:
		return GST_VIDEO_FORMAT_UNKNOWN;
	}
}

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer user_data)
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

static int scanfilter(const struct dirent *entry)
{
	return g_str_has_suffix(entry->d_name, "-render");
}

static gchar *get_device_name(gchar *device_name)
{
	struct dirent **list;
	int n = scandir("/dev/dri/by-path/", &list, scanfilter, versionsort);
	gchar *ret = NULL;

	if (n == -1) {
		return "-- Unknown Name --";
	}

	struct pci_access *pci = pci_alloc();
	pci_init(pci);

	for (int i = 0; i < n && ret == NULL; i++) {
		char device[1024] = {};
		int domain, bus, dev, fun;

		sscanf(list[i]->d_name, "%*[^-]-%x:%x:%x.%x%*s", &domain, &bus, &dev, &fun);

		struct pci_dev *pci_dev = pci_get_dev(pci, domain, bus, dev, fun);
		if (pci_dev == NULL) {
			continue;
		}

		pci_fill_info(pci_dev, PCI_FILL_IDENT);
		pci_lookup_name(pci, device, sizeof(device), PCI_LOOKUP_DEVICE, pci_dev->vendor_id, pci_dev->device_id);

		gchar *path = g_strdup_printf("/dev/dri/by-path/%s", list[i]->d_name);
		char *dev_name = realpath(path, NULL);
		g_free(path);

		if (g_strcmp0(basename(dev_name), device_name) == 0) {
			ret = g_strdup(device);
		}
		free(dev_name);

		pci_free_dev(pci_dev);
	}

	pci_cleanup(pci);

	while (n--) {
		free(list[n]);
	}
	free(list);

	return ret;
}

static const char *get_name(void *type_data)
{
	gchar **fields = g_regex_split_simple("obs-va-(tex-)?va(renderD\\d+)?(h264|h265|av1)(lp)?enc", type_data, 0, 0);
	gchar *devname = get_device_name(g_strcmp0(fields[2], "") == 0 ? "renderD128" : fields[2]);
	gchar *name = g_strdup_printf("VAAPI %s%s on %s%s",
				      g_strcmp0(fields[3], "h264") == 0   ? "H.264"
				      : g_strcmp0(fields[3], "h265") == 0 ? "H.265"
									  : "AV1",
				      g_strcmp0(fields[1], "tex-") == 0 ? " (Texture)" : "", devname,
				      g_strcmp0(fields[4], "lp") == 0 ? " (Low Power)" : "");

	g_free(devname);
	g_strfreev(fields);

	gpointer val = g_hash_table_lookup(hash_table, name);
	if (val != NULL) {
		g_free(name);
		return val;
	}
	g_hash_table_insert(hash_table, name, name);
	return name;
}

static gchar *get_device_suffix(const char *encoder_id)
{
	gchar **fields = g_regex_split_simple("obs-va-(?:tex-)?va(renderD\\d+)?.*", encoder_id, 0, 0);
	gchar *suffix = g_strdup(fields[1] != NULL ? fields[1] : "");

	g_strfreev(fields);

	return suffix;
}

static gchar *get_device_path(const char *encoder_id)
{
	gchar *suffix = get_device_suffix(encoder_id);
	gchar *path = g_strdup_printf("/dev/dri/%s", *suffix == '\0' ? "renderD128" : suffix);

	g_free(suffix);

	return path;
}

static gchar *get_postproc_name(const char *encoder_id)
{
	gchar *suffix = get_device_suffix(encoder_id);
	gchar *name = g_strdup_printf("va%spostproc", suffix);

	g_free(suffix);

	return name;
}

static bool setup_texture(obs_vaapi_t *vaapi)
{
	video_t *video = obs_encoder_video(vaapi->encoder);
	if (video == NULL) {
		blog(LOG_ERROR, "[obs-vaapi] encoder has no video");
		return false;
	}

	const struct video_output_info *voi = video_output_get_info(video);

	GstVideoFormat format;

	switch (voi->format) {
	case VIDEO_FORMAT_NV12:
		format = GST_VIDEO_FORMAT_NV12;
		break;
	case VIDEO_FORMAT_P010:
		format = GST_VIDEO_FORMAT_P010_10LE;
		break;
	default:
		blog(LOG_ERROR, "[obs-vaapi] unsupported texture format: %d", voi->format);
		return false;
	}

	gchar *device = get_device_path(obs_encoder_get_id(vaapi->encoder));

	vaapi->va_display = gst_va_display_drm_new_from_path(device);
	g_free(device);
	if (vaapi->va_display == NULL) {
		blog(LOG_ERROR, "[obs-vaapi] failed to open VA display");
		return false;
	}

	guint usage_hint = VA_SURFACE_ATTRIB_USAGE_HINT_VPP_READ | VA_SURFACE_ATTRIB_USAGE_HINT_VPP_WRITE;

	gst_video_info_dma_drm_init(&vaapi->drm_info);
	gst_video_info_set_format(&vaapi->drm_info.vinfo, format, obs_encoder_get_width(vaapi->encoder),
				  obs_encoder_get_height(vaapi->encoder));
	GST_VIDEO_INFO_FPS_N(&vaapi->drm_info.vinfo) = vaapi->video_info.fps_num;
	GST_VIDEO_INFO_FPS_D(&vaapi->drm_info.vinfo) = vaapi->video_info.fps_den;

	vaapi->drm_info.drm_fourcc = gst_video_dma_drm_fourcc_from_format(format);
	vaapi->drm_info.drm_modifier = gst_va_dmabuf_get_modifier_for_format(vaapi->va_display, format, usage_hint);
	if (vaapi->drm_info.drm_modifier == DRM_FORMAT_MOD_INVALID) {
		blog(LOG_ERROR, "[obs-vaapi] failed to query dmabuf modifier");
		return false;
	}

	vaapi->va_allocator = gst_va_dmabuf_allocator_new(vaapi->va_display);
	if (vaapi->va_allocator == NULL ||
	    !gst_va_dmabuf_allocator_set_format(vaapi->va_allocator, &vaapi->drm_info, usage_hint)) {
		blog(LOG_ERROR, "[obs-vaapi] failed to set up dmabuf allocator");
		return false;
	}

	return true;
}

static void *create(obs_data_t *settings, obs_encoder_t *encoder)
{
	obs_vaapi_t *vaapi = bzalloc(sizeof(obs_vaapi_t));

	vaapi->encoder = encoder;

	obs_get_video_info(&vaapi->video_info);

	gboolean texture = g_str_has_prefix(obs_encoder_get_id(encoder), TEXTURE_PREFIX);

	GstCaps *caps = NULL;

	if (texture) {
		if (!setup_texture(vaapi)) {
			if (vaapi->va_allocator) {
				gst_object_unref(vaapi->va_allocator);
			}
			if (vaapi->va_display) {
				gst_object_unref(vaapi->va_display);
			}
			bfree(vaapi);
			return NULL;
		}

		caps = gst_video_info_dma_drm_to_caps(&vaapi->drm_info);
	} else {
		caps = gst_caps_new_simple("video/x-raw", "framerate", GST_TYPE_FRACTION, vaapi->video_info.fps_num,
					   vaapi->video_info.fps_den, "width", G_TYPE_INT,
					   obs_encoder_get_width(encoder), "height", G_TYPE_INT,
					   obs_encoder_get_height(encoder), "interlace-mode", G_TYPE_STRING,
					   "progressive", NULL);

		switch (vaapi->video_info.output_format) {
		case VIDEO_FORMAT_I420:
			gst_caps_set_simple(caps, "format", G_TYPE_STRING, "I420", NULL);
			break;
		case VIDEO_FORMAT_NV12:
			gst_caps_set_simple(caps, "format", G_TYPE_STRING, "NV12", NULL);
			break;
		case VIDEO_FORMAT_I444:
			gst_caps_set_simple(caps, "format", G_TYPE_STRING, "Y444", NULL);
			break;
		case VIDEO_FORMAT_BGRA:
			gst_caps_set_simple(caps, "format", G_TYPE_STRING, "BGRx", NULL);
			break;
		case VIDEO_FORMAT_P010:
			gst_caps_set_simple(caps, "format", G_TYPE_STRING, "P010_10LE", NULL);
			break;
		default:
			blog(LOG_ERROR, "[obs-vaapi] unsupported color format: %d", vaapi->video_info.output_format);
			gst_caps_unref(caps);
			bfree(vaapi);
			return NULL;
		}
	}

	vaapi->pipe = gst_pipeline_new(NULL);
	vaapi->appsrc = gst_element_factory_make("appsrc", NULL);
	vaapi->appsink = gst_element_factory_make("appsink", NULL);

	gst_util_set_object_arg(G_OBJECT(vaapi->appsrc), "format", "time");

	// Should never trigger as we block the encode function
	// until the current buffer has been consumed. If we
	// block for too long it should be reported as encoder
	// overload in OBS.
	g_signal_connect(vaapi->appsrc, "enough-data", G_CALLBACK(enough_data), NULL);

	g_object_set(vaapi->appsink, "sync", FALSE, NULL);

	GstVideoColorimetry cinfo;

	cinfo.range = vaapi->video_info.range == VIDEO_RANGE_FULL ? GST_VIDEO_COLOR_RANGE_0_255
								  : GST_VIDEO_COLOR_RANGE_16_235;

	switch (vaapi->video_info.colorspace) {
	case VIDEO_CS_601:
		cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT601;
		cinfo.transfer = GST_VIDEO_TRANSFER_BT601;
		cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_SMPTE170M;
		break;
	default:
	case VIDEO_CS_709:
		cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
		cinfo.transfer = GST_VIDEO_TRANSFER_BT709;
		cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
		break;
	case VIDEO_CS_SRGB:
		cinfo.matrix = GST_VIDEO_COLOR_MATRIX_RGB;
		cinfo.transfer = GST_VIDEO_TRANSFER_SRGB;
		cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_BT709;
		break;
	case VIDEO_CS_2100_PQ:
		cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
		cinfo.transfer = GST_VIDEO_TRANSFER_SMPTE2084;
		cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
		break;
	case VIDEO_CS_2100_HLG:
		cinfo.matrix = GST_VIDEO_COLOR_MATRIX_BT2020;
		cinfo.transfer = GST_VIDEO_TRANSFER_ARIB_STD_B67;
		cinfo.primaries = GST_VIDEO_COLOR_PRIMARIES_BT2020;
		break;
	}
	gst_caps_set_simple(caps, "colorimetry", G_TYPE_STRING, gst_video_colorimetry_to_string(&cinfo), NULL);

	g_object_set(vaapi->appsrc, "caps", caps, NULL);
	gst_caps_unref(caps);

	GstElement *vaapipostproc = NULL;
	GstElement *vaapiencoder = NULL;
	GstElement *parser = NULL;

	gchar *postproc_name = get_postproc_name(obs_encoder_get_id(encoder));

	vaapipostproc = gst_element_factory_make(postproc_name, NULL);
	g_free(postproc_name);

	if (texture) {
		vaapiencoder = gst_element_factory_make(obs_encoder_get_id(encoder) + strlen(TEXTURE_PREFIX), NULL);
	} else {
		vaapiencoder = gst_element_factory_make(obs_encoder_get_id(encoder) + strlen("obs-va-"), NULL);
	}

	gst_util_set_object_arg(G_OBJECT(vaapipostproc), "scale-method", "hq");

	if (texture) {
		g_object_set(vaapipostproc, "disable-passthrough", TRUE, NULL);
	}

	if (g_strcmp0(obs_encoder_get_codec(encoder), "h264") == 0) {
		parser = gst_element_factory_make("h264parse", NULL);

		caps = gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
					   G_TYPE_STRING, "au", NULL);

		g_object_set(vaapi->appsink, "caps", caps, NULL);
		gst_caps_unref(caps);
	} else if (g_strcmp0(obs_encoder_get_codec(encoder), "hevc") == 0) {
		parser = gst_element_factory_make("h265parse", NULL);

		caps = gst_caps_new_simple("video/x-h265", "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
					   G_TYPE_STRING, "au", NULL);

		g_object_set(vaapi->appsink, "caps", caps, NULL);
		gst_caps_unref(caps);
	} else {
		parser = gst_element_factory_make("av1parse", NULL);

		caps = gst_caps_new_simple("video/x-av1", "stream-format", G_TYPE_STRING, "obu-stream", "alignment",
					   G_TYPE_STRING, "tu", NULL);

		g_object_set(vaapi->appsink, "caps", caps, NULL);
		gst_caps_unref(caps);
	}

	gst_bin_add_many(GST_BIN(vaapi->pipe), vaapi->appsrc, vaapipostproc, vaapiencoder, parser, vaapi->appsink,
			 NULL);
	gst_element_link_many(vaapi->appsrc, vaapipostproc, vaapiencoder, parser, vaapi->appsink, NULL);

	obs_properties_t *properties = obs_encoder_properties(encoder);
	for (obs_property_t *property = obs_properties_first(properties); property; obs_property_next(&property)) {
		const char *name = obs_property_name(property);
		switch (obs_property_get_type(property)) {
		case OBS_PROPERTY_TEXT:
			gst_util_set_object_arg(G_OBJECT(vaapiencoder), name, obs_data_get_string(settings, name));
			blog(LOG_INFO, "[obs-vaapi] %s: %s", name, obs_data_get_string(settings, name));
			break;
		case OBS_PROPERTY_INT:
			g_object_set(vaapiencoder, name, obs_data_get_int(settings, name), NULL);
			blog(LOG_INFO, "[obs-vaapi] %s: %lld", name, obs_data_get_int(settings, name));
			break;
		case OBS_PROPERTY_BOOL:
			g_object_set(vaapiencoder, name, obs_data_get_bool(settings, name), NULL);
			blog(LOG_INFO, "[obs-vaapi] %s: %d", name, obs_data_get_bool(settings, name));
			break;
		case OBS_PROPERTY_FLOAT:
			g_object_set(vaapiencoder, name, obs_data_get_double(settings, name), NULL);
			blog(LOG_INFO, "[obs-vaapi] %s: %f", name, obs_data_get_double(settings, name));
			break;
		case OBS_PROPERTY_LIST:
			gst_util_set_object_arg(G_OBJECT(vaapiencoder), name, obs_data_get_string(settings, name));
			blog(LOG_INFO, "[obs-vaapi] %s: %s", name, obs_data_get_string(settings, name));
			break;
		default:
			blog(LOG_WARNING, "[obs-vaapi] unhandled property: %s", name);
			break;
		}
	}
	obs_properties_destroy(properties);

	GstBus *bus = gst_element_get_bus(vaapi->pipe);
	gst_bus_add_watch(bus, bus_callback, NULL);
	gst_object_unref(bus);

	blog(LOG_INFO, "[obs-vaapi] codec: %s, %dx%d@%d/%d, format: %s ", obs_encoder_get_id(encoder),
	     obs_encoder_get_width(encoder), obs_encoder_get_height(encoder), vaapi->video_info.fps_num,
	     vaapi->video_info.fps_den, gst_video_format_to_string(map_video_format(vaapi->video_info.output_format)));

	gst_element_set_state(vaapi->pipe, GST_STATE_PLAYING);

	g_mutex_init(&vaapi->mutex);
	g_cond_init(&vaapi->cond);

	return vaapi;
}

static void destroy(void *data)
{
	obs_vaapi_t *vaapi = data;

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

	if (vaapi->va_allocator) {
		gst_object_unref(vaapi->va_allocator);
	}
	if (vaapi->va_display) {
		gst_object_unref(vaapi->va_display);
	}

	g_mutex_clear(&vaapi->mutex);
	g_cond_clear(&vaapi->cond);

	bfree(vaapi->codec_data);
	bfree(vaapi);
}

static void destroy_notify(void *data)
{
	obs_vaapi_t *vaapi = data;

	g_mutex_lock(&vaapi->mutex);
	vaapi->consumed = true;
	g_cond_signal(&vaapi->cond);
	g_mutex_unlock(&vaapi->mutex);
}

static void buffer_destroy_notify(gpointer data, GstMiniObject *object)
{
	UNUSED_PARAMETER(object);

	destroy_notify(data);
}

static void release_sample(obs_vaapi_t *vaapi)
{
	if (vaapi->sample) {
		GstBuffer *buffer = gst_sample_get_buffer(vaapi->sample);
		gst_buffer_unmap(buffer, &vaapi->info);
		gst_sample_unref(vaapi->sample);
		vaapi->sample = NULL;
	}
}

static bool copy_texture(obs_vaapi_t *vaapi, GstBuffer *buffer, struct encoder_texture *texture)
{
	VASurfaceID surface = gst_va_buffer_get_surface(buffer);
	VADisplay display = gst_va_display_get_va_dpy(vaapi->va_display);
	VADRMPRIMESurfaceDescriptor desc = {0};

	if (surface == VA_INVALID_ID) {
		blog(LOG_ERROR, "[obs-vaapi] buffer has no VA surface");
		return false;
	}

	VAStatus status = vaExportSurfaceHandle(display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
						VA_EXPORT_SURFACE_SEPARATE_LAYERS | VA_EXPORT_SURFACE_READ_WRITE,
						&desc);
	if (status != VA_STATUS_SUCCESS) {
		blog(LOG_ERROR, "[obs-vaapi] failed to export VA surface: %s", vaErrorStr(status));
		return false;
	}

	bool ok = true;

	for (guint32 i = 0; i < desc.num_layers && ok; i++) {
		if (texture->tex[i] == NULL) {
			blog(LOG_ERROR, "[obs-vaapi] unexpected number of textures");
			ok = false;
			break;
		}

		uint32_t width = desc.width;
		uint32_t height = desc.height;

		if (i) {
			width /= 2;
			height /= 2;
		}

		uint32_t object = desc.layers[i].object_index[0];
		int fd = desc.objects[object].fd;
		uint32_t stride = desc.layers[i].pitch[0];
		uint32_t offset = desc.layers[i].offset[0];
		uint64_t modifier = desc.objects[object].drm_format_modifier;

		gs_texture_t *tex = gs_texture_create_from_dmabuf(width, height, desc.layers[i].drm_format,
								  drm_to_gs_color_format(desc.layers[i].drm_format), 1,
								  &fd, &stride, &offset, &modifier);
		if (tex == NULL) {
			blog(LOG_ERROR, "[obs-vaapi] failed to import dmabuf plane %u", i);
			ok = false;
			break;
		}

		gs_copy_texture(tex, texture->tex[i]);
		gs_texture_destroy(tex);
	}

	for (guint32 i = 0; i < desc.num_objects; i++) {
		close(desc.objects[i].fd);
	}

	return ok;
}

static bool encode_receive(obs_vaapi_t *vaapi, GstBuffer *buffer, struct encoder_packet *packet, bool *received_packet)
{
	g_mutex_lock(&vaapi->mutex);
	vaapi->consumed = false;
	g_mutex_unlock(&vaapi->mutex);

	GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(vaapi->appsrc), buffer);
	if (ret != GST_FLOW_OK) {
		return false;
	}

	g_mutex_lock(&vaapi->mutex);
	while (!vaapi->consumed) {
		g_cond_wait(&vaapi->cond, &vaapi->mutex);
	}
	g_mutex_unlock(&vaapi->mutex);

	vaapi->sample = gst_app_sink_try_pull_sample(GST_APP_SINK(vaapi->appsink), 0);
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

	packet->pts /= GST_SECOND / (packet->timebase_den / packet->timebase_num);
	packet->dts /= GST_SECOND / (packet->timebase_den / packet->timebase_num);

	packet->type = OBS_ENCODER_VIDEO;

	packet->keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

	return true;
}

static bool encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received_packet)
{
	obs_vaapi_t *vaapi = data;

	*received_packet = false;

	release_sample(vaapi);

	GstVideoFormat format = GST_VIDEO_FORMAT_UNKNOWN;
	gsize buffer_size = 0;

	switch (vaapi->video_info.output_format) {
	case VIDEO_FORMAT_I420:
		format = GST_VIDEO_FORMAT_I420;
		buffer_size = obs_encoder_get_width(vaapi->encoder) * obs_encoder_get_height(vaapi->encoder) * 3 / 2;
		break;
	case VIDEO_FORMAT_NV12:
		format = GST_VIDEO_FORMAT_NV12;
		buffer_size = obs_encoder_get_width(vaapi->encoder) * obs_encoder_get_height(vaapi->encoder) * 3 / 2;
		break;
	case VIDEO_FORMAT_I444:
		format = GST_VIDEO_FORMAT_Y444;
		buffer_size = obs_encoder_get_width(vaapi->encoder) * obs_encoder_get_height(vaapi->encoder) * 3;
		break;
	case VIDEO_FORMAT_BGRA:
		format = GST_VIDEO_FORMAT_BGRx;
		buffer_size = obs_encoder_get_width(vaapi->encoder) * obs_encoder_get_height(vaapi->encoder) * 4;
		break;
	case VIDEO_FORMAT_P010:
		format = GST_VIDEO_FORMAT_P010_10LE;
		buffer_size = obs_encoder_get_width(vaapi->encoder) * obs_encoder_get_height(vaapi->encoder) * 3;
		break;
	case VIDEO_FORMAT_I010:
		format = GST_VIDEO_FORMAT_I420_10LE;
		buffer_size = obs_encoder_get_width(vaapi->encoder) * obs_encoder_get_height(vaapi->encoder) * 3;
		break;
	default:
		return false;
	}

	GstBuffer *buffer =
		gst_buffer_new_wrapped_full(0, frame->data[0], buffer_size, 0, buffer_size, vaapi, destroy_notify);

	GstVideoMeta *meta = gst_buffer_add_video_meta(buffer, 0, format, obs_encoder_get_width(vaapi->encoder),
						       obs_encoder_get_height(vaapi->encoder));

	for (int i = 0; frame->linesize[i]; i++) {
		meta->stride[i] = frame->linesize[i];
	}

	GST_BUFFER_PTS(buffer) = frame->pts * (GST_SECOND / (packet->timebase_den / packet->timebase_num));

	return encode_receive(vaapi, buffer, packet, received_packet);
}

static bool encode_texture2(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key,
			    uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	obs_vaapi_t *vaapi = data;

	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);

	*received_packet = false;

	release_sample(vaapi);

	GstBuffer *buffer = gst_buffer_new();
	if (!gst_va_dmabuf_allocator_prepare_buffer(vaapi->va_allocator, buffer) &&
	    !gst_va_dmabuf_allocator_setup_buffer(vaapi->va_allocator, buffer)) {
		blog(LOG_ERROR, "[obs-vaapi] failed to set up dmabuf buffer");
		gst_buffer_unref(buffer);
		return false;
	}

	gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_INFO_FORMAT(&vaapi->drm_info.vinfo),
				       GST_VIDEO_INFO_WIDTH(&vaapi->drm_info.vinfo),
				       GST_VIDEO_INFO_HEIGHT(&vaapi->drm_info.vinfo),
				       GST_VIDEO_INFO_N_PLANES(&vaapi->drm_info.vinfo), vaapi->drm_info.vinfo.offset,
				       vaapi->drm_info.vinfo.stride);

	obs_enter_graphics();

	bool ok = copy_texture(vaapi, buffer, texture);

	gs_flush();

	obs_leave_graphics();

	if (!ok) {
		gst_buffer_unref(buffer);
		return false;
	}

	GST_BUFFER_PTS(buffer) = pts * (GST_SECOND / (packet->timebase_den / packet->timebase_num));

	gst_mini_object_weak_ref(GST_MINI_OBJECT(buffer), buffer_destroy_notify, vaapi);

	return encode_receive(vaapi, buffer, packet, received_packet);
}

static void get_defaults2(obs_data_t *settings, void *type_data)
{
	GstElement *encoder = NULL;

	if (g_str_has_prefix(type_data, TEXTURE_PREFIX)) {
		encoder = gst_element_factory_make(type_data + strlen(TEXTURE_PREFIX), NULL);
	} else {
		encoder = gst_element_factory_make(type_data + strlen("obs-va-"), NULL);
	}

	guint num_properties;
	GParamSpec **property_specs = g_object_class_list_properties(G_OBJECT_GET_CLASS(encoder), &num_properties);

	for (guint i = 0; i < num_properties; i++) {
		GParamSpec *param = property_specs[i];

		if (param->owner_type == G_TYPE_OBJECT || param->owner_type == GST_TYPE_OBJECT ||
		    param->owner_type == GST_TYPE_PAD || (param->flags & G_PARAM_WRITABLE) == 0) {
			continue;
		}

		GValue value = {
			0,
		};
		g_value_init(&value, param->value_type);

		g_object_get_property(G_OBJECT(encoder), param->name, &value);

		switch (G_VALUE_TYPE(&value)) {
		case G_TYPE_STRING:
			obs_data_set_default_string(settings, param->name, g_value_get_string(&value));
			break;
		case G_TYPE_UINT64:
			obs_data_set_default_int(settings, param->name, g_value_get_uint64(&value));
			break;
		case G_TYPE_INT64:
			obs_data_set_default_int(settings, param->name, g_value_get_int64(&value));
			break;
		case G_TYPE_UINT:
			obs_data_set_default_int(settings, param->name, g_value_get_uint(&value));
			break;
		case G_TYPE_INT:
			obs_data_set_default_int(settings, param->name, g_value_get_int(&value));
			break;
		case G_TYPE_BOOLEAN:
			obs_data_set_default_bool(settings, param->name, g_value_get_boolean(&value));
			break;
		case G_TYPE_FLOAT:
			obs_data_set_default_double(settings, param->name, g_value_get_float(&value));
			break;
		case G_TYPE_DOUBLE:
			obs_data_set_default_double(settings, param->name, g_value_get_double(&value));
			break;
		default:
			if (G_IS_PARAM_SPEC_ENUM(param)) {
				GEnumValue *values = G_ENUM_CLASS(g_type_class_ref(param->value_type))->values;
				gint enum_value = g_value_get_enum(&value);
				for (int j = 0; values[j].value_name; j++) {
					if (values[j].value == enum_value) {
						obs_data_set_default_string(settings, param->name,
									    values[j].value_name);
						break;
					}
				}
			} else if (GST_IS_PARAM_SPEC_ARRAY_LIST(param)) {
				obs_data_set_default_string(settings, param->name, gst_value_serialize(&value));
			} else if (G_VALUE_TYPE(&value) == GST_TYPE_STRUCTURE) {
				const GstStructure *s = gst_value_get_structure(&value);
				if (s != NULL) {
					gchar *str = gst_structure_serialize(s, GST_SERIALIZE_FLAG_NONE);
					obs_data_set_default_string(settings, param->name, str);
					g_free(str);
				}
			} else {
				blog(LOG_WARNING, "[obs-vaapi] unhandled property: %s", param->name);
			}
			break;
		}
	}

	g_free(property_specs);
	gst_object_unref(encoder);
}

static obs_properties_t *get_properties2(void *data, void *type_data)
{
	GstElement *encoder = NULL;

	if (g_str_has_prefix(type_data, TEXTURE_PREFIX)) {
		encoder = gst_element_factory_make(type_data + strlen(TEXTURE_PREFIX), NULL);
	} else {
		encoder = gst_element_factory_make(type_data + strlen("obs-va-"), NULL);
	}

	obs_property_t *property = NULL;

	obs_properties_t *properties = obs_properties_create();

	guint num_properties;
	GParamSpec **property_specs = g_object_class_list_properties(G_OBJECT_GET_CLASS(encoder), &num_properties);

	for (guint i = 0; i < num_properties; i++) {
		GParamSpec *param = property_specs[i];

		if (param->owner_type == G_TYPE_OBJECT || param->owner_type == GST_TYPE_OBJECT ||
		    param->owner_type == GST_TYPE_PAD || (param->flags & G_PARAM_WRITABLE) == 0) {
			continue;
		}

		GValue value = {
			0,
		};
		g_value_init(&value, param->value_type);

		g_object_get_property(G_OBJECT(encoder), param->name, &value);

		switch (G_VALUE_TYPE(&value)) {
		case G_TYPE_STRING:
			property = obs_properties_add_text(properties, param->name, param->name, OBS_TEXT_DEFAULT);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_UINT64:
			property = obs_properties_add_int(properties, param->name, param->name,
							  G_PARAM_SPEC_UINT64(param)->minimum,
							  MIN(G_PARAM_SPEC_UINT64(param)->maximum, G_MAXINT32), 1);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_INT64:
			property = obs_properties_add_int(properties, param->name, param->name,
							  G_PARAM_SPEC_INT64(param)->minimum,
							  MIN(G_PARAM_SPEC_INT64(param)->maximum, G_MAXINT32), 1);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_UINT:
			property = obs_properties_add_int(properties, param->name, param->name,
							  G_PARAM_SPEC_UINT(param)->minimum,
							  MIN(G_PARAM_SPEC_UINT(param)->maximum, G_MAXINT32), 1);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_INT:
			property = obs_properties_add_int(properties, param->name, param->name,
							  G_PARAM_SPEC_INT(param)->minimum,
							  G_PARAM_SPEC_INT(param)->maximum, 1);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_BOOLEAN:
			property = obs_properties_add_bool(properties, param->name, param->name);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_FLOAT:
			property = obs_properties_add_float(properties, param->name, param->name,
							    G_PARAM_SPEC_FLOAT(param)->minimum,
							    G_PARAM_SPEC_FLOAT(param)->maximum, 0.1);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		case G_TYPE_DOUBLE:
			property = obs_properties_add_float(properties, param->name, param->name,
							    G_PARAM_SPEC_DOUBLE(param)->minimum,
							    G_PARAM_SPEC_DOUBLE(param)->maximum, 0.1);
			obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			break;
		default:
			if (G_IS_PARAM_SPEC_ENUM(param)) {
				property = obs_properties_add_list(properties, param->name, param->name,
								   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
				GEnumValue *values = G_ENUM_CLASS(g_type_class_ref(param->value_type))->values;
				for (int j = 0; values[j].value_name; j++) {
					obs_property_list_add_string(property, values[j].value_name,
								     values[j].value_nick);
				}
				obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			} else if (GST_IS_PARAM_SPEC_ARRAY_LIST(param)) {
				property =
					obs_properties_add_text(properties, param->name, param->name, OBS_TEXT_DEFAULT);
				obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			} else if (G_VALUE_TYPE(&value) == GST_TYPE_STRUCTURE) {
				property =
					obs_properties_add_text(properties, param->name, param->name, OBS_TEXT_DEFAULT);
				obs_property_set_long_description(property, g_param_spec_get_blurb(param));
			} else {
				blog(LOG_WARNING, "[obs-vaapi] unhandled property: %s", param->name);
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
	obs_vaapi_t *vaapi = data;

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

extern const char *obs_vaapi_version;

MODULE_EXPORT bool obs_module_load(void)
{
	guint major, minor, micro, nano;

	gst_version(&major, &minor, &micro, &nano);

	blog(LOG_INFO, "[obs-vaapi] version: %s, gst-runtime: %u.%u.%u", obs_vaapi_version, major, minor, micro);

	gst_init(NULL, NULL);

	hash_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	struct obs_encoder_info vaapi = {
		.type = OBS_ENCODER_VIDEO,
		.get_name = get_name,
		.create = create,
		.destroy = destroy,
		.get_defaults2 = get_defaults2,
		.get_properties2 = get_properties2,
		.encode = encode,
		.get_extra_data = get_extra_data,
	};

	GList *list = gst_registry_get_feature_list_by_plugin(gst_registry_get(), "va");

	for (GList *elem = list; elem != NULL; elem = elem->next) {
		GstPluginFeature *feature = elem->data;

		gchar **fields = g_regex_split_simple("va(renderD\\d+)?(h264|h265|av1)(lp)?enc",
						      gst_plugin_feature_get_name(feature), 0, 0);
		if (g_strcmp0(fields[0], "") != 0) {
			g_strfreev(fields);
			continue;
		}
		if (g_strcmp0(fields[2], "h264") == 0) {
			vaapi.codec = "h264";
		} else if (g_strcmp0(fields[2], "h265") == 0) {
			vaapi.codec = "hevc";
		} else {
			vaapi.codec = "av1";
		}
		g_strfreev(fields);

		vaapi.encode = encode;
		vaapi.encode_texture2 = NULL;
		vaapi.caps = 0;

		vaapi.id = vaapi.type_data = g_strdup_printf("obs-va-%s", gst_plugin_feature_get_name(feature));
		g_hash_table_insert(hash_table, vaapi.type_data, vaapi.type_data);
		obs_register_encoder(&vaapi);

		vaapi.encode = NULL;
		vaapi.encode_texture2 = encode_texture2;
		vaapi.caps = OBS_ENCODER_CAP_PASS_TEXTURE;

		vaapi.id = vaapi.type_data = g_strdup_printf(TEXTURE_PREFIX "%s", gst_plugin_feature_get_name(feature));
		g_hash_table_insert(hash_table, vaapi.type_data, vaapi.type_data);
		obs_register_encoder(&vaapi);

		blog(LOG_INFO, "[obs-vaapi] found %s", gst_plugin_feature_get_name(feature));
	}

	gst_plugin_feature_list_free(list);

	return true;
}

MODULE_EXPORT void obs_module_unload(void)
{
	g_hash_table_unref(hash_table);
}
