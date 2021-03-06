/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
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

#include "config.h"
#include "VolumeFilterPlugin.hxx"
#include "FilterPlugin.hxx"
#include "FilterInternal.hxx"
#include "FilterRegistry.hxx"
#include "conf.h"
#include "pcm_buffer.h"
#include "PcmVolume.hxx"
#include "audio_format.h"

#include <assert.h>
#include <string.h>

class VolumeFilter final : public Filter {
	/**
	 * The current volume, from 0 to #PCM_VOLUME_1.
	 */
	unsigned volume;

	struct audio_format format;

	struct pcm_buffer buffer;

public:
	VolumeFilter()
		:volume(PCM_VOLUME_1) {}

	unsigned GetVolume() const {
		assert(volume <= PCM_VOLUME_1);

		return volume;
	}

	void SetVolume(unsigned _volume) {
		assert(_volume <= PCM_VOLUME_1);

		volume = _volume;
	}

	virtual const audio_format *Open(audio_format &af, GError **error_r);
	virtual void Close();
	virtual const void *FilterPCM(const void *src, size_t src_size,
				      size_t *dest_size_r, GError **error_r);
};

static inline GQuark
volume_quark(void)
{
	return g_quark_from_static_string("pcm_volume");
}

static Filter *
volume_filter_init(gcc_unused const struct config_param *param,
		   gcc_unused GError **error_r)
{
	return new VolumeFilter();
}

const struct audio_format *
VolumeFilter::Open(audio_format &audio_format, gcc_unused GError **error_r)
{
	format = audio_format;
	pcm_buffer_init(&buffer);

	return &format;
}

void
VolumeFilter::Close()
{
	pcm_buffer_deinit(&buffer);
}

const void *
VolumeFilter::FilterPCM(const void *src, size_t src_size,
			size_t *dest_size_r, GError **error_r)
{
	*dest_size_r = src_size;

	if (volume >= PCM_VOLUME_1)
		/* optimized special case: 100% volume = no-op */
		return src;

	void *dest = pcm_buffer_get(&buffer, src_size);

	if (volume <= 0) {
		/* optimized special case: 0% volume = memset(0) */
		/* XXX is this valid for all sample formats? What
		   about floating point? */
		memset(dest, 0, src_size);
		return dest;
	}

	memcpy(dest, src, src_size);

	bool success = pcm_volume(dest, src_size,
				  sample_format(format.format),
				  volume);
	if (!success) {
		g_set_error(error_r, volume_quark(), 0,
			    "pcm_volume() has failed");
		return NULL;
	}

	return dest;
}

const struct filter_plugin volume_filter_plugin = {
	"volume",
	volume_filter_init,
};

unsigned
volume_filter_get(const Filter *_filter)
{
	const VolumeFilter *filter =
		(const VolumeFilter *)_filter;

	return filter->GetVolume();
}

void
volume_filter_set(Filter *_filter, unsigned volume)
{
	VolumeFilter *filter = (VolumeFilter *)_filter;

	filter->SetVolume(volume);
}

