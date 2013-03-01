/*
 * Copyright (C) 2003-2012 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Note: Implementation details taken from DarkIce project:
 * https://code.google.com/p/darkice
 */

#include "config.h"
#include "AacPlusEncoderPlugin.hxx"
#include "encoder_api.h"
#include "encoder_plugin.h"
#include "audio_format.h"
#include "mpd_error.h"
#include <aacplus.h>

#include <assert.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "aacplus_encoder"

#define AAC_BITRATE_AUTO 64000
#define AAC_BITRATE_MAX  64000

struct AacPlusEncoder final {
	/** the base class */
	struct encoder encoder;

	/* configuration */
	unsigned long bitrate;        /* in bits per second */
	
	/* TODO 
	int   bandWidth;
	*/

	/* runtime information */
	unsigned input_samples;
	unsigned max_output_bytes;
	unsigned output_size;
	unsigned write_pos;
	unsigned read_pos;
	struct audio_format audio_format;
	aacplusEncHandle aacplus;
	unsigned char *output_buffer;
	
	AacPlusEncoder();
	bool Configure(const struct config_param *param, GError **error_r);
	void Reset();
	bool Open(struct audio_format *af, GError **error_r);
	bool Write(const void *data, size_t length, GError **error_r);
	size_t Read(void *dest, size_t length);
};

static inline GQuark
aacplus_encoder_quark(void)
{
	return g_quark_from_static_string("aacplus_encoder");
}

#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

static inline constexpr AacPlusEncoder *
Cast(struct encoder *_encoder)
{
	return (AacPlusEncoder *)
		((char *)_encoder - offsetof(AacPlusEncoder, encoder));
}

#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

AacPlusEncoder::AacPlusEncoder() :
	bitrate(64000), 
	input_samples(0),
	max_output_bytes(0),
	output_size(0),
	write_pos(0),
	read_pos(0),
	aacplus(nullptr),
	output_buffer(nullptr) {
	encoder_struct_init(&encoder, &aacplus_encoder_plugin);
	audio_format_clear(&audio_format);
}

inline bool
AacPlusEncoder::Configure(G_GNUC_UNUSED const struct config_param *param,
	G_GNUC_UNUSED GError **error_r)
{
	const char *value = config_get_block_string(param, "bitrate", "auto");
	if (strcmp(value, "auto") == 0)
		bitrate = AAC_BITRATE_AUTO;
	else if (strcmp(value, "max") == 0)
		bitrate = AAC_BITRATE_MAX;
	else {
		char *endptr;
		bitrate = strtoul(value, &endptr, 10);
		if (endptr == value || *endptr != 0 ||
		    bitrate < 500 || bitrate > 512000) {
			g_set_error(error_r, aacplus_encoder_quark(), 0,
				"Invalid bit rate");
			return false;
		}
	}
	return true;
}

inline void
AacPlusEncoder::Reset()
{
	if (aacplus) {
		aacplusEncClose(aacplus);
		aacplus = nullptr;
	}
	if (output_buffer) {
		g_free(output_buffer);
		output_buffer = nullptr;
	}
	max_output_bytes = 0;
	output_size = 0;
	write_pos = 0;
	read_pos = 0;
}

static struct encoder *
aacplus_encoder_init(const struct config_param *param, GError **error_r)
{
	AacPlusEncoder *encoder = new AacPlusEncoder();

	/* load configuration from "param" */
	if (!encoder->Configure(param, error_r)) {
		/* configuration has failed, roll back and return error */
		delete encoder;
		return nullptr;
	}

	return &encoder->encoder;
}

static void
aacplus_encoder_finish(struct encoder *_encoder)
{
	AacPlusEncoder *encoder = Cast(_encoder);

	/* the real libaacplus cleanup was already performed by
	   aacplus_encoder_close(), so no real work here */
	delete encoder;
}

inline bool
AacPlusEncoder::Open(struct audio_format *af, GError **error_r)
{
	aacplusEncConfiguration *aacplus_config;
	unsigned long samples = 0;
	unsigned long max_bytes = 0;
	assert(aacplus == nullptr);
	assert(output_buffer == nullptr);

	af->channels = 2;
	if (af->format != SAMPLE_FORMAT_FLOAT && 
		af->format != SAMPLE_FORMAT_S16) {
		g_warning("Setting output format to 16, was %s",
			sample_format_to_string((sample_format)af->format));
		af->format = SAMPLE_FORMAT_S16;
	}

	aacplus = aacplusEncOpen(af->sample_rate,
		af->channels,
		&samples,
		&max_bytes);
	if (!aacplus) {
		g_set_error(error_r, aacplus_encoder_quark(), 0,
			"error opening aacplus handle");
		return false;
	}
	if (samples > UINT_MAX || max_bytes > UINT_MAX) {
		g_set_error(error_r, aacplus_encoder_quark(), 0,
			"required output samples or buffer size too large");
		return false;
	}
	aacplus_config = aacplusEncGetCurrentConfiguration(aacplus);
	aacplus_config->bitRate      = (int)bitrate; 
	aacplus_config->bandWidth    = 0; /* lowpass frequency cutoff */
	aacplus_config->outputFormat = 1; /* ADTS frames */
	aacplus_config->nChannelsOut = 2;
	if (af->format == SAMPLE_FORMAT_FLOAT)
		aacplus_config->inputFormat = AACPLUS_INPUT_FLOAT;
	else if (af->format == SAMPLE_FORMAT_S16)
		aacplus_config->inputFormat = AACPLUS_INPUT_16BIT;

	if (!aacplusEncSetConfiguration(aacplus, aacplus_config)) {
		g_set_error(error_r, aacplus_encoder_quark(), 0,
			"error configuring libaacplus library");
		return false;
	}
	
	/* save for later use */
	audio_format = *af;
	input_samples = (unsigned)samples;
	max_output_bytes = (unsigned)max_bytes;
	
	g_debug("aacplus config:");
	g_debug(".sampleRate %d", aacplus_config->sampleRate);
	g_debug(".bitRate %d", aacplus_config->bitRate);
	g_debug(".nChannelsIn %d", aacplus_config->nChannelsIn);
	g_debug(".nChannelsOut %d", aacplus_config->nChannelsOut);
	g_debug(".bandWidth %d", aacplus_config->bandWidth);
	g_debug(".inputFormat %d", aacplus_config->inputFormat);
	g_debug(".outputFormat %d", aacplus_config->outputFormat);
	g_debug(".nSamplesPerFrame %d", aacplus_config->nSamplesPerFrame);
	g_debug(".inputSamples %d", aacplus_config->inputSamples);
	g_debug("encoder input_samples %u max_output_bytes %u",
		input_samples, max_output_bytes);
		
	output_buffer = (unsigned char *)g_malloc(max_output_bytes);
	if (!output_buffer) {
		g_set_error(error_r, aacplus_encoder_quark(), 0,
			"not enough memory for output buffer");
		return false;
	}
	write_pos = 0;
	read_pos = 0;
	
	return true;
}

static bool
aacplus_encoder_open(struct encoder *_encoder,
		    struct audio_format *af,
		    GError **error_r)
{
	AacPlusEncoder *encoder = Cast(_encoder);
	return encoder->Open(af, error_r);
}

static void
aacplus_encoder_close(struct encoder *_encoder)
{
	AacPlusEncoder *encoder = Cast(_encoder);
	encoder->Reset();
}

inline bool
AacPlusEncoder::Write(const void *data, size_t length, GError **error_r)
{
	const unsigned char *b = (const unsigned char *)data; 
	unsigned sample_size = audio_format_frame_size(&audio_format);
	unsigned total_samples = length / sample_size;
	unsigned processed_samples = 0;
	assert(aacplus != nullptr);
	
	g_debug("AacPlusEncoder::Write length=%u total_samples=%u max_samples=%u",
		length, total_samples, input_samples);
	while (processed_samples < total_samples) {
		int out_bytes;
		unsigned size_needed; 
		unsigned char *output_bufp;
		unsigned samples = total_samples - processed_samples;
		if (samples > input_samples)
			samples = input_samples;

		/* aacplusEncEncode uses non-const input */
#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
		/* typo in DarkIce code calculating buffer pointer? */
		int32_t *input_bufp = 
			(int32_t *)(b + processed_samples * sample_size);
#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

		size_needed = max_output_bytes + write_pos;
		if (size_needed < max_output_bytes) {
			/* wrap around */
			g_set_error(error_r, aacplus_encoder_quark(), 0,
				"buffer allocation size too large");
			return false;
		}
		if (size_needed > output_size) {
			unsigned char* new_buffer = (unsigned char *)
				g_try_realloc(output_buffer, size_needed);
			if (!new_buffer) {
				g_set_error(error_r, aacplus_encoder_quark(), 0,
					"not enough memory to enlarge output buffer");
				return false;
			}
			output_buffer = new_buffer;
			output_size = size_needed;
		}
		output_bufp = &output_buffer[write_pos];
		
		out_bytes = aacplusEncEncode(aacplus,
			input_bufp,
			samples,
			output_bufp,
			max_output_bytes);
	
		if (out_bytes <= 0) {
			g_set_error(error_r, aacplus_encoder_quark(), 0,
				"aacPlusEncEncode returned %d", out_bytes);
			return false;
		}
		write_pos += out_bytes;
		processed_samples += samples;
		g_debug("AacPlusEncoder::Write out_bytes=%d read_pos %u write_pos %u",
			out_bytes, read_pos, write_pos);
	}
	return true;
}

static bool
aacplus_encoder_write(struct encoder *_encoder,
	const void *data, size_t length, GError **error_r)
{
	AacPlusEncoder *encoder = Cast(_encoder);
	return encoder->Write(data, length, error_r);
}
		
inline size_t
AacPlusEncoder::Read(void *dest, size_t length)
{
	size_t available;
	if (length == 0 || read_pos >= write_pos)
		return 0;
	
	available = write_pos - read_pos;
	if (length > available)
		length = available;
	
	g_debug("AacPlusEncoder::Read before read_pos %u write_pos %u length %u",
		read_pos, write_pos, length);
	memcpy(dest, &output_buffer[read_pos], length);
	write_pos = available - length;
	if (read_pos > 0) {
		if (length < available)
			g_memmove(output_buffer, 
				&output_buffer[read_pos + length],
				write_pos);
		read_pos = 0;
	}
	g_debug("AacPlusEncoder::Read after  read_pos %u write_pos %u",
		read_pos, write_pos);
	
	return length;
}

static size_t
aacplus_encoder_read(struct encoder *_encoder, void *dest, size_t length)
{
	AacPlusEncoder *encoder = Cast(_encoder);
	return encoder->Read(dest, length);
}

static const char *
aacplus_encoder_get_mime_type(G_GNUC_UNUSED struct encoder *_encoder)
{
	return  "audio/aac";
}

const struct encoder_plugin aacplus_encoder_plugin = {
	"aacplus",
	aacplus_encoder_init,
	aacplus_encoder_finish,
	aacplus_encoder_open,
	aacplus_encoder_close,
	nullptr, /* end */
	nullptr, /* flush */
	nullptr, /* pre_tag */
	nullptr, /* tag */
	aacplus_encoder_write,
	aacplus_encoder_read,
	aacplus_encoder_get_mime_type,
};
