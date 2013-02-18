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
#include "HlsOutputPlugin.hxx"
#include "output_api.h"
#include "thread/Mutex.hxx"

#include <assert.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "hls_output"

/**
 * The quark used for GError.domain.
 */
static inline GQuark
hls_output_quark(void)
{
	return g_quark_from_static_string("hls_output");
}

struct HlsOutput final {
	struct audio_output base;

	/**
	 * This mutex protects the playlist state.
	 */
	mutable Mutex mutex;

	/**
	 * True if the audio output is open and accepts client
	 * connections.
	 */
	bool open;
		
	/**
	 * Configuration parameters.
	 */
	
	/**
	 * Output directory for index file
	 */
	char *index_file_dir;
	
	/**
	 * File name of index file, default 'prog_index.m3u8'
	 */
	char *index_file_name;
	
	/**
	 * Base URL for segments.
	 */
	char *segment_base_url;
	
	/**
	 * Output directory for segment files
	 */
	char *segment_file_dir;
	
	/**
	 * Base name of segment file, default 'sequence'.
	 */
	char *segment_file_base;

	/**
	 * Extension of segment file, default '.mp3'
	 */
	char *segment_file_ext;
	
	/**
	 * Maximum length of segment in seconds.
	 */
	unsigned segment_duration;
	
	/**
	 * Number of segments to show in index file
	 */
	unsigned segment_window_size;
	
	/**
	 * Number of expired segments to keep; -1 to keep all
	 */
	int segment_history_size;
		
	HlsOutput();
	~HlsOutput();

	bool Configure(const config_param *param, GError **error_r);
	bool Bind(GError **error_r);
	void Unbind();
	bool Open(struct audio_format *audio_format, GError **error_r);
	void Close();
	unsigned Delay();
	size_t Play(const void *chunk, size_t size, GError **error_r);
	bool Pause();
	void SendTag(const struct tag *tag);
	void Cancel();
};

HlsOutput::HlsOutput()
{
	
}

HlsOutput::~HlsOutput()
{
}

inline bool
HlsOutput::Configure(const config_param *param, GError **error_r)
{
	GError *error = NULL;
	
	/* read configuration */
	index_file_dir    = config_dup_block_path(param, "index_file_dir", &error);
	if (!index_file_dir) {
		if (error != NULL)
			g_propagate_error(error_r, error);
		else
			g_set_error(error_r, hls_output_quark(), 0,
				    "No \"index_file_dir\" parameter specified");
		return false;
	}
	
	segment_file_dir  = config_dup_block_path(param, "segment_file_dir", error_r);
	if (!segment_file_dir) {
		if (error != NULL)
			g_propagate_error(error_r, error);
		else
			g_set_error(error_r, hls_output_quark(), 0,
				    "No \"segment_file_dir\" parameter specified");
		return false;
	}
	
	segment_base_url  = config_dup_block_string(param, "segment_base_url", NULL);
	if (!segment_base_url) 
	{
		g_set_error(error_r, hls_output_quark(), 0,
				    "No \"segment_base_url\" parameter specified");
		return false;
	}
	
	index_file_name   = config_dup_block_string(param, "index_file_name", "prog_index.m3u8");
	segment_file_base = config_dup_block_string(param, "segment_file_base", "sequence");
	segment_file_ext  = config_dup_block_string(param, "segment_file_ext", "mp3");
	segment_duration     = config_get_block_unsigned(param, "segment_duration", 10);	
	segment_window_size  = config_get_block_unsigned(param, "segment_window_size", 5);
	segment_history_size = config_get_block_unsigned(param, "segment_history_size",  5);

	return true;
}

static struct audio_output *
hls_output_init(const struct config_param *param,
		  GError **error_r)
{
	HlsOutput *hls = new HlsOutput();

	if (!ao_base_init(&hls->base, &hls_output_plugin, param,
			  error_r)) {
		delete hls;
		return nullptr;
	}

	if (!hls->Configure(param, error_r)) {
		ao_base_finish(&hls->base);
		delete hls;
		return nullptr;
	}

	return &hls->base;
}

#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

static inline constexpr HlsOutput *
Cast(audio_output *ao)
{
	return (HlsOutput *)((char *)ao - offsetof(HlsOutput, base));
}

#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static void
hls_output_finish(struct audio_output *ao)
{
	HlsOutput *hls = Cast(ao);

	ao_base_finish(&hls->base);
	delete hls;
}

inline bool
HlsOutput::Bind(GError **error_r __attribute__((unused))) 
{
	return true;
}

static bool
hls_output_enable(struct audio_output *ao, GError **error_r)
{
	HlsOutput *hls = Cast(ao);

	return hls->Bind(error_r);
}

inline void
HlsOutput::Unbind()
{
	
}

static void
hls_output_disable(struct audio_output *ao)
{
	HlsOutput *hls = Cast(ao);

	hls->Unbind();
}

inline bool
HlsOutput::Open(struct audio_format *audio_format __attribute__((unused)),
	GError **error_r __attribute__((unused)))
{
	assert(!open);

	/* open the encoder */

	/* initialize other attributes */

	open = true;
	return true;
}

static bool
hls_output_open(struct audio_output *ao, struct audio_format *audio_format,
	GError **error)
{
	HlsOutput *hls = Cast(ao);

	const ScopeLock protect(hls->mutex);
	return hls->Open(audio_format, error);
}

inline void
HlsOutput::Close()
{
	assert(open);

	open = false;
}

static void
hls_output_close(struct audio_output *ao)
{
	HlsOutput *hls = Cast(ao);

	const ScopeLock protect(hls->mutex);
	hls->Close();
}

inline unsigned
HlsOutput::Delay()
{
	/* some arbitrary delay that is long enough to avoid
	   consuming too much CPU, and short enough to notice
	   new clients quickly enough */
	return 1000; // TODO
}

static unsigned
hls_output_delay(struct audio_output *ao)
{
	HlsOutput *hls = Cast(ao);
	
	return hls->Delay();
}

inline size_t
HlsOutput::Play(const void *chunk __attribute__((unused)), size_t size, 
	GError **error_r  __attribute__((unused)))
{
	size = 0; // TODO
	return size;
}

static size_t
hls_output_play(struct audio_output *ao, const void *chunk, size_t size,
	GError **error_r)
{
	HlsOutput *hls = Cast(ao);

	return hls->Play(chunk, size, error_r);
}

inline bool
HlsOutput::Pause()
{
	return true;
}

static bool
hls_output_pause(struct audio_output *ao)
{
	HlsOutput *hls = Cast(ao);
	
	return hls->Pause();
}

inline void
HlsOutput::SendTag(const struct tag *tag __attribute__((unused)))
{
	assert(tag != NULL);
	
	// TODO: add tag to metadata, then use WebVTT to 
	// expose metadata in playlist
}

static void
hls_output_tag(struct audio_output *ao, const struct tag *tag)
{
	HlsOutput *hls = Cast(ao);

	hls->SendTag(tag);
}

inline void
HlsOutput::Cancel()
{

}

static void
hls_output_cancel(struct audio_output *ao)
{
	HlsOutput *hls = Cast(ao);
	
	hls->Cancel();
}

const struct audio_output_plugin hls_output_plugin = {
	"hls",
	nullptr,
	hls_output_init,
	hls_output_finish,
	hls_output_enable,
	hls_output_disable,
	hls_output_open,
	hls_output_close,
	hls_output_delay,
	hls_output_tag,
	hls_output_play,
	nullptr,
	hls_output_cancel,
	hls_output_pause,
	nullptr,
};
