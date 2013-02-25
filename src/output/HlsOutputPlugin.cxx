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
#include "encoder_plugin.h"
#include "encoder_list.h"
#include "PlayerControl.hxx"
#include "fd_util.h"
#include "mpd_error.h"
#include "timer.h"

#include <vector>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <glib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "hls_output"

#define HLS_PROTOCOL_VERSION 4

/**
 * The quark used for GError.domain.
 */
static inline GQuark
hls_output_quark(void)
{
	return g_quark_from_static_string("hls_output");
}

enum HlsPlaylistType {
	PLAYLIST_TYPE_LIVE,
	PLAYLIST_TYPE_EVENT,
	PLAYLIST_TYPE_VOD
};

#define ENCODER_COPY_BUFFER_SIZE 8192
#define FILE_COPY_BUFFER_SIZE 8192

struct HlsSegment final {

	/**
	 * Sequence number, set by segmenter.
	 */
	unsigned seq;
	
	/**
	 * File handle.
	 */
	int fd;
	
	/**
	 * ISO-formatted start time, set by segmenter.
	 */
	char* program_datetime;
	
	/**
	 * Human-readable title.
	 */
	char* title;
	
	/**
	 * Bytes encoded to segment.
	 */
	size_t encoded;
	
	/**
	 * Duration of segment.
	 */
	float duration;
	
	HlsSegment(unsigned seq_no, GTimeVal *start_time, const char *seg_title);
	~HlsSegment();
	
	inline bool IsOpen() { return fd >= 0; }
	bool CheckDuration(size_t size, unsigned time_rate, 
		unsigned target_duration);
	int Open(struct HlsSegmenter* segmenter, GError **error_r);
	size_t Encode(struct encoder *encoder, const void *chunk, size_t size, GError **error_r);
	size_t FlushEncodedData(struct encoder* encoder, GError **error_r);
	void Close();
	bool Finish(struct encoder* encoder, unsigned time_rate);
};

struct HlsSegmenter final {
	struct audio_output base;
	
	/**
	 * HLS protocol version;
	 */
	int version;

	/**
	 * Instruct HLS clients to allow cache;
	 */
	bool allow_cache;

	/**
	 * Playlist type: "LIVE", "EVENT" or "VOD".
	 * Currently only supports "LIVE" playlists, where the m3u8
	 * index file is rewritten as new segments are available.
	 * If you choose "EVENT" or "VOD", the index file will never
	 * be written until the output is stopped.
	 * TODO: Implement EVENT and VOD playlist types.
	 * For EVENT type, the m3u8 is only appended to (not rewritten).
     * An event playlist looks just like a live playlist to start out with. 
     * It doesn't initially have an EXT-X-ENDLIST tag, indicating that new
     * media files will be added to the playlist as they become available.
     * New segments are added until the event has concluded, at which time 
     * the EXT-X-ENDLIST tag is appended.
	 * For VOD type, the m3u8 is fixed (only written when done).
     * The index file is static and contains a complete list of URLs 
     * to all media files created since the beginning of the presentation.
     * This kind of session allows the client full access to the entire
     * program.
	 */
	HlsPlaylistType playlist_type;
	
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
	 * Bitrate for encoding.
	 */
	unsigned bitrate;
	
	/**
	 * Maximum length of segment in seconds.
	 */
	unsigned target_duration;
	
	/**
	 * Number of segments to show in index file
	 */
	unsigned window_size;
	
	/**
	 * Number of expired segments to keep; 0 to keep all
	 */
	unsigned history_size;
	
	/**
	 * State parameters
	 */
	
	/**
	 * Window - segments in history, segments to be indexed, and
	 * current open segment.
	 */
	std::vector<struct HlsSegment*> segments;
	
	/**
	 * Audio format for encoder (set on first open)
	*/
	audio_format encoder_format;
	
	/**
	 * MP3 or AAC encoder plugin instance.
	*/
	struct encoder *encoder;
	
	/**
	 * Synchronization timer for live playlist index.
	*/
	struct timer *timer;
	
	/**
	 * Full path to index file, default 'prog_index.m3u8'
	 */
	char *index_file_path;
	
	/**
	 * True if the audio output is open and accepts client
	 * connections.
	 */
	bool open;

	/**
	 * True if encoder has been opened.
	 */
	bool encoder_opened;
		
	/**
	 * True if header written (EVENT or VOD playlist).
	 */
	bool append_to_index;
	
	/**
	 * True if we unlink expired segments.
	 */
	bool unlink_expired_segments;
	
	/**
	 * Timer to make sure we don't go over duration.
	 */
	unsigned time_rate;
	
	/**
	 * Last X-EXT-MEDIA-SEQUENCE value written to index file.
	 */
	unsigned media_seq;
	
	/**
	 * Last X-EXT-MEDIA-SEQUENCE segment created.
	 */
	unsigned window_top_seq;
	
	/**
	 * Index of first segment in window to be indexed.
	 */
	unsigned history_top;
	
	/**
	 * Index of current open segment.
	 */
	unsigned window_top;
	
	
	HlsSegmenter();
	~HlsSegmenter();
	
	bool ConfigureEncoder(const config_param *param, GError **error_r);
	const char* MediaSegmentPath(char* buf, size_t bufsiz, 
		const HlsSegment *segment);
	const char* MediaSegmentURL(char* buf, size_t bufsiz,
		const HlsSegment *segment);
	HlsSegment* ActiveSegment();
	HlsSegment* StartNewSegment(GTimeVal* start_time, const char *seg_title,
		GError **error_r);
	void FinishActiveSegment();
	void RemoveSegment(unsigned i);
	bool WriteIndexFile(bool finished, GError **error_r);
	bool UpdateWindowAndIndexFile(bool finished, GError **error_r);
	
	bool Configure(const config_param *param, GError **error_r);
	void Stop();
	void Finish();
	bool Bind(GError **error_r);
	void Unbind();
	bool Open(struct audio_format *af, GError **error_r);
	void Close();
	unsigned Delay();
	size_t Play(const void *chunk, size_t size, GError **error_r);
	bool Pause();
	void SendTag(const struct tag *tag);
	void Cancel();
};


/*
 * HlsSegment class implementation.
 */
inline HlsSegment::HlsSegment(unsigned seq_no, GTimeVal *start_time, 
	const char *seg_title) : 
	seq(seq_no),
	fd(-1),
	program_datetime(nullptr),
	title(nullptr),
	encoded(0),
	duration(0.)
{
	program_datetime = g_time_val_to_iso8601(start_time);
	title = g_strdup(seg_title);
}

HlsSegment::~HlsSegment()
{
	g_free(program_datetime);
	g_free(title);
}

inline bool 
HlsSegment::CheckDuration(size_t size, unsigned time_rate, 
	unsigned target_duration)
{
	/* We calculate in millisecs -- good enough */
	unsigned check = (unsigned)(((uint64_t)(encoded + size) * 1000) /
		time_rate);
	unsigned target = target_duration * 1000;
	if (check > target) {
		g_debug("Segment::CheckDuration %u target_duration %u exceeded",
			check, target);
		return false;
	}
	return true;
}

inline int 
HlsSegment::Open(struct HlsSegmenter* segmenter, GError **error_r)
{
	if (fd < 0) {
		char buf[256];
		const char* segment_path = segmenter->MediaSegmentPath(buf,
			sizeof(buf), this);
		
		if (segment_path)
			fd = open_cloexec(segment_path, O_CREAT|O_WRONLY|O_TRUNC, 0666);
		if (fd < 0)
			g_set_error(error_r, hls_output_quark(), 0,
				"Cannot open segment seq %u at '%s'", seq, segment_path);
		else
			g_debug("Segment::Open seq %u opened at '%s' with fd %d", 
				seq, segment_path, fd);
	}
	return fd;
}

inline void 
HlsSegment::Close()
{
	if (fd >= 0) {
		close(fd);
		fd = -1;
	}
}

inline size_t
HlsSegment::Encode(struct encoder *encoder, const void *chunk, size_t size, GError **error_r)
{
	bool ok = encoder_write(encoder, chunk, size, error_r);
	encoded += size;
	return ok ? size : 0;
}

inline size_t 
HlsSegment::FlushEncodedData(struct encoder* encoder, GError **error_r)
{
	size_t length, total;
	static char buffer[ENCODER_COPY_BUFFER_SIZE];	

	if (fd < 0) {
		g_set_error(error_r, hls_output_quark(), 0,
			"Cannot write data: segment not open");
		return 0;
	}
	
	total = 0;
	while ((length = encoder_read(encoder, buffer, sizeof(buffer))) > 0) {
		G_GNUC_UNUSED ssize_t ignored = write(fd, buffer, length);
		total += length;
	}
	return total;
}	

/*
 * Returns true if encoder was closed.
 */
inline bool
HlsSegment::Finish(struct encoder* encoder, unsigned time_rate)
{
	GError *error = nullptr;
	
	duration = (float)encoded/(float)time_rate;
	encoder_end(encoder, &error);
	FlushEncodedData(encoder, &error);
	Close();
	encoder_close(encoder);
	return true;
}

/*
 * HlsSegmenter class implementation.
 */
#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif

static inline constexpr HlsSegmenter *
Cast(audio_output *ao)
{
	return (HlsSegmenter *)((char *)ao - offsetof(HlsSegmenter, base));
}

#if GCC_CHECK_VERSION(4,6) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

inline HlsSegmenter::HlsSegmenter() :
	version(HLS_PROTOCOL_VERSION),
	allow_cache(false),
	playlist_type(PLAYLIST_TYPE_LIVE),
	segment_base_url(nullptr),
	segment_file_dir(nullptr),
	segment_file_base(nullptr),
	segment_file_ext(nullptr),
	bitrate(128),
	target_duration(10),
	window_size(5),
	history_size(5),
	encoder(nullptr),
	timer(nullptr),
	index_file_path(nullptr),
	open(false),
	encoder_opened(false),
	append_to_index(false),
	unlink_expired_segments(false),
	time_rate(88200),
	media_seq(0),
	window_top_seq(0),
	history_top(0),
	window_top(0)
{
	audio_format_clear(&encoder_format);
}

HlsSegmenter::~HlsSegmenter()
{
	g_free(index_file_path);
	g_free(segment_base_url);
	g_free(segment_file_dir);
	g_free(segment_file_base);
	g_free(segment_file_ext);
}

inline bool 
HlsSegmenter::ConfigureEncoder(const config_param *param, GError **error_r)
{
	const char* encoder_name = nullptr;
	if (strcmp(segment_file_ext, "aac") == 0)
		encoder_name = "aacplus";
	else if (strcmp(segment_file_ext, "mp3") == 0)
		encoder_name = "lame";
	if (!encoder_name) 
	{
		g_set_error(error_r, hls_output_quark(), 0,
			    "Invalid \"segment_file_ext\" parameter specified. Must be aac or mp3.");
		return false;
	}
	const struct encoder_plugin *encoder_plugin = encoder_plugin_get(encoder_name);
	if (!encoder_plugin) 
	{
		g_set_error(error_r, hls_output_quark(), 0,
			    "No encoder available for specified \"segment_file_ext\" parameter.");
		return false;
	}
	encoder = encoder_init(encoder_plugin, param, error_r);
	if (!encoder)
		return false;
	
	return true;
}

inline const char* HlsSegmenter::MediaSegmentPath(char* buf, size_t bufsiz, 
	const HlsSegment* segment)
{
	int ret = g_snprintf(buf, bufsiz, "%s/%s%u.%s", 
		segment_file_dir, segment_file_base, segment->seq, segment_file_ext);
	if (ret < 0 || (size_t) ret > bufsiz) 
		return nullptr;
	return buf;
}

inline const char* HlsSegmenter::MediaSegmentURL(char* buf, size_t bufsiz,
	const HlsSegment* segment)
{
	int ret = g_snprintf(buf, bufsiz, "%s/%s%u.%s", 
		segment_base_url, segment_file_base, segment->seq, segment_file_ext);
	if (ret < 0 || (size_t) ret > bufsiz)
		return nullptr;
	return buf;
}

inline void
HlsSegmenter::RemoveSegment(unsigned i)
{
	
	assert(i < segments.size());
	HlsSegment* segment = segments[i];
	assert(!segment->IsOpen());
	
	g_debug("Segmenter::RemoveSegment %u seq=%u", i, segment->seq);
	if (unlink_expired_segments) {
		char buf[256];
		const char* segment_path = MediaSegmentPath(buf, 
			sizeof(buf), segment);
		unlink(segment_path);
	}
	segments.erase(segments.begin()+i);
	delete segment;
	
	if (i < history_top)
		history_top--;
	if (i < window_top)
		window_top--;
}


inline bool
HlsSegmenter::WriteIndexFile(bool finished, GError **error_r) 
{
	static char buffer[FILE_COPY_BUFFER_SIZE];	
	char url_buf[512];
	int fd_src, fd_dst;
	unsigned i, temp_media_seq;
	size_t copied;
	const char *mode;
	bool rv = false;
	char *temp_path = nullptr;
	FILE* f = nullptr;
	HlsSegment* segment = nullptr;
	bool first = !append_to_index;
	
	if (history_top >= window_top || history_top >= segments.size()) {
		g_set_error(error_r, hls_output_quark(), 0,
			"Nothing to index - history_top %u out of range", history_top);
		return false;
	}
		
	temp_media_seq = segments[history_top]->seq;
	
	if (playlist_type == PLAYLIST_TYPE_LIVE) {
		temp_path = g_strdup_printf("%s.%u", 
			index_file_path, temp_media_seq);
		fd_dst = open_cloexec(temp_path, O_CREAT|O_WRONLY|O_TRUNC, 0666);
		mode = "w";
	} else {
		fd_dst = open_cloexec(index_file_path, O_CREAT|O_WRONLY|O_APPEND, 0666);
		mode = "a";
	}
	
	if (fd_dst >= 0)	
	 	f = fdopen(fd_dst, mode);
	if (!f) {
		g_set_error(error_r, hls_output_quark(), 0,
			"Cannnot open index file '%s'", 
			temp_path ? temp_path : index_file_path);
	} else {
		rv = true;
		if (!append_to_index) {
			fprintf(f, "#EXTM3U\n");
			fprintf(f, "#EXT-X-VERSION:%d\n", version);
			fprintf(f, "#EXT-X-TARGETDURATION:%u\n", target_duration); 
		    fprintf(f, "#EXT-X-ALLOW-CACHE:%s\n", allow_cache ? "YES" : "NO"); 
			fprintf(f, "#EXT-X-MEDIA-SEQUENCE:%u\n", temp_media_seq);
			if (playlist_type == PLAYLIST_TYPE_EVENT || 
				playlist_type == PLAYLIST_TYPE_VOD)
				append_to_index = true;
		}

	   	/**
	      * TODO: segment program datetime
	      * IETF draft says this is only informative.
	      * Only print this for first segment, or for a segment with an
	      * EXT-X-DISCONTINUITY tag applied to it.
	      */
		for (i = history_top; i < window_top; i++) {
			segment = segments[i];
			if (first) {
		    	fprintf(f, "#EXT-X-PROGRAM-DATE-TIME:%s\n",
					segment->program_datetime);
				first = false;
			}
			/* TODO: segment title with artist, etc */
			fprintf(f, "#EXTINF:%0.2f,%s\n",
				segment->duration, "" /* segment->title */); 
			MediaSegmentURL(url_buf, sizeof(url_buf), segment);
			fprintf(f, "%s\n", url_buf);
		}
	
		if (finished) {
			fprintf(f, "#EXT-X-ENDLIST\n");
		}
		fflush(f);
		fclose(f);

		/* copy m3u8.0 temp file to m3u8 */
		if (temp_path) {
			fd_src = open_cloexec(temp_path, O_RDONLY, 0666);
			fd_dst = open_cloexec(index_file_path, 
				O_CREAT|O_WRONLY|O_TRUNC, 0666);
			if (fd_src >= 0 && fd_dst >= 0) {
				while ((copied = read(fd_src, buffer, sizeof(buffer))) != 0) {
					write(fd_dst, buffer, copied);
				}
			} else {
				rv = false;
			}
			if (fd_src >= 0)
				close(fd_src);
			if (fd_dst >= 0)
				close(fd_dst);
		}
	}
	if (temp_path)
		g_free(temp_path);
	if (rv)
		media_seq = temp_media_seq;
	return rv;
}

inline bool
HlsSegmenter::UpdateWindowAndIndexFile(bool finished, GError **error_r)
{
	size_t active_size = segments.size() - history_top;
	assert(segments.size() >= history_top);
	
	if (active_size > (window_size + 1)) {
		g_debug("Segmenter::Update before ms=%u size=%u ht=%u wt=%u",
			media_seq, segments.size(), history_top, window_top);
		g_debug("Segmenter::Update active_size=%u window_size=%u",
			active_size, window_size);
		
		if (!WriteIndexFile(finished, error_r))
			return false;
		if (playlist_type == PLAYLIST_TYPE_LIVE) {
			if (history_top >= history_size) {
				size_t to_delete = history_top - history_size + 1;
				size_t i = 0;
				for (i = 0; i < to_delete; i++) {
					RemoveSegment(0);
				}
			}
			history_top++;
			g_debug("Segmenter::Update after  ms=%u size=%u ht=%u wt=%u",
				media_seq, segments.size(), history_top, window_top);
		}
	}
	return true;
}


inline HlsSegment* 
HlsSegmenter::ActiveSegment()
{
	if (window_top >= segments.size())
		return nullptr;
	return segments[window_top];
}

inline HlsSegment* 
HlsSegmenter::StartNewSegment(GTimeVal* start_time, const char* seg_title,
	GError **error_r)
{
	g_debug("Segmenter::StartNewSegment");
	HlsSegment* new_segment = new HlsSegment(window_top_seq, 
		start_time, seg_title);
	if (!new_segment->Open(this, error_r)) {
		delete new_segment;
		return nullptr;
	}
	if (!encoder_opened) {
		if (!encoder_open(encoder, &encoder_format, error_r)) {
			delete new_segment;
			return nullptr;
		}
		encoder_opened = true;
	}
	segments.push_back(new_segment);
	window_top = segments.size() - 1;
	window_top_seq++;
	return new_segment;
}

inline void
HlsSegmenter::FinishActiveSegment()
{
	g_debug("Segmenter::FinishActiveSegment");
	HlsSegment* active = ActiveSegment();
	if (active) {
		if (active->Finish(encoder, time_rate))
			encoder_opened = false;
		window_top++;
	}
}

inline bool
HlsSegmenter::Configure(const config_param *param, GError **error_r)
{
	struct audio_format_string af_string;
	GError *error = nullptr;
	char *index_file_dir;
	const char *index_file_name;
	
	/* read configuration */
	const char *pl_type = config_get_block_string(param, 
		"playlist_type", "LIVE");
	if (strcmp(pl_type, "LIVE") == 0)
		playlist_type = PLAYLIST_TYPE_LIVE;
	else if (strcmp(pl_type, "EVENT") == 0)
		playlist_type = PLAYLIST_TYPE_EVENT;
	else if (strcmp(pl_type, "VOD") == 0) 
		playlist_type = PLAYLIST_TYPE_EVENT;
	else {
		g_set_error(error_r, hls_output_quark(), 0,
			"Invalid playlist_type parameter specified");
		return false;	
	}
	allow_cache = config_get_block_bool(param, "allow_cache", false);

	index_file_dir = config_dup_block_path(param, 
		"index_file_dir", &error);
	if (!index_file_dir) {
		if (error != nullptr)
			g_propagate_error(error_r, error);
		else
			g_set_error(error_r, hls_output_quark(), 0,
				    "No \"index_file_dir\" parameter specified");
		return false;
	}
	index_file_name = config_get_block_string(param, 
		"index_file_name", "prog_index.m3u8");
	index_file_path = g_strdup_printf("%s/%s", 
		index_file_dir, index_file_name);
	g_free(index_file_dir);
	
	segment_file_dir = config_dup_block_path(param, 
		"segment_file_dir", error_r);
	if (!segment_file_dir) {
		if (error != nullptr)
			g_propagate_error(error_r, error);
		else
			g_set_error(error_r, hls_output_quark(), 0,
				    "No \"segment_file_dir\" parameter specified");
		return false;
	}
	
	segment_base_url = config_dup_block_string(param, 
		"segment_base_url", nullptr);
	if (!segment_base_url) {
		g_set_error(error_r, hls_output_quark(), 0,
				    "No \"segment_base_url\" parameter specified");
		return false;
	}

	bitrate = config_get_block_unsigned(param, "bitrate", 128);
	if (bitrate == 0) {
		g_set_error(error_r, hls_output_quark(), 0,
				    "No \"bitrate\" parameter specified");
		return false;
	}
	segment_file_ext = config_dup_block_string(param, 
		"segment_file_ext", "mp3");
	if (!ConfigureEncoder(param, error_r))
		return false;
	
	segment_file_base = config_dup_block_string(param, 
		"segment_file_base", "sequence");
	target_duration = config_get_block_unsigned(param, 
		"target_duration", 10);	
	window_size  = config_get_block_unsigned(param, "window_size", 5);
	history_size = config_get_block_unsigned(param, "history_size",  5);

	g_debug("Segmenter::Configure in  format: %s",
		audio_format_to_string(&base.in_audio_format, &af_string));
	g_debug("Segmenter::Configure out format: %s",
		audio_format_to_string(&base.out_audio_format, &af_string));
	return true;
}

static struct audio_output *
hls_output_init(const struct config_param *param,
		  GError **error_r)
{
	HlsSegmenter *hls = new HlsSegmenter();
	if (!ao_base_init(&hls->base, &hls_output_plugin, param, error_r)) {
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

inline void
HlsSegmenter::Stop()
{
	/* This is where we write out VOD playlists */
	GError *error = nullptr;
	timer_reset(timer);
	FinishActiveSegment();
	UpdateWindowAndIndexFile(true, &error);
}

inline void 
HlsSegmenter::Finish()
{
	g_debug("Segmenter::Finish");
	Stop();
	encoder_finish(encoder);
	encoder = nullptr;	
	ao_base_finish(&base);
}

static void
hls_output_finish(struct audio_output *ao)
{
	HlsSegmenter *hls = Cast(ao);
	hls->Finish();
	delete hls;
}

inline bool
HlsSegmenter::Bind(G_GNUC_UNUSED GError **error_r) 
{
	g_debug("Segmenter::Bind");
	return true;
}

static bool
hls_output_enable(struct audio_output *ao, GError **error_r)
{
	HlsSegmenter *hls = Cast(ao);
	return hls->Bind(error_r);
}

inline void
HlsSegmenter::Unbind()
{
	g_debug("Segmenter::Unbind");
}

static void
hls_output_disable(struct audio_output *ao)
{
	HlsSegmenter *hls = Cast(ao);

	hls->Unbind();
}

inline bool
HlsSegmenter::Open(struct audio_format *af, GError **error_r)
{
	struct audio_format_string af_string;
	assert(!open);
	assert(!encoder_opened);
	
	if (!encoder_open(encoder, af, error_r))
		return false;
	time_rate = af->sample_rate * audio_format_frame_size(af);
	if (time_rate == 0) {
		g_set_error(error_r, hls_output_quark(), 0,
			"Invalid encoder_format");
		encoder_close(encoder);
		return false;
	}
	g_debug("Segmenter::Open encoder_format %s",
		audio_format_to_string(af, &af_string));
	g_debug("Segmenter::Open time_rate %u", time_rate);
	
	timer = timer_new(af);
	encoder_format = *af; /* save format for reopening segments */
	encoder_opened = true;
	open = true;
	return true;
}

static bool
hls_output_open(struct audio_output *ao, struct audio_format *af,
	GError **error_r)
{
	HlsSegmenter *hls = Cast(ao);
	return hls->Open(af, error_r);
}

inline void
HlsSegmenter::Close()
{
	g_debug("Segmenter::Close");
	assert(open);

	FinishActiveSegment();
	open = false;
}

static void
hls_output_close(struct audio_output *ao)
{
	HlsSegmenter *hls = Cast(ao);
	hls->Close();
}

inline unsigned
HlsSegmenter::Delay() 
{
	/* Calculate delay if we are rewriting the 
	   m3u8 index file in real time. */
	if (playlist_type == PLAYLIST_TYPE_LIVE && timer->started) {
		unsigned delay = timer_delay(timer);
		g_debug("Segmenter::Delay time %llu delay %u msec",
			timer->time, delay);
		return delay;
	}
	return 0;
}

static unsigned
hls_output_delay(G_GNUC_UNUSED struct audio_output *ao)
{
	HlsSegmenter *hls = Cast(ao);
	return hls->Delay();
}

inline size_t 
HlsSegmenter::Play(const void *chunk, size_t size, GError **error_r)
{
	if (playlist_type == PLAYLIST_TYPE_LIVE) {
		if (!timer->started)
			timer_start(timer);
		timer_add(timer, size);
	}
	
	HlsSegment* active = ActiveSegment();
	if (active) {
		/* see if this segment can accept more data */
		if (!active->CheckDuration(size, time_rate, target_duration)) {
			FinishActiveSegment();
			g_debug("Segmenter::WriteData segment %u finished", active->seq);
			active = nullptr;
		}
	}
	
	if (!active) {
		/* TODO: get the actual program time */
		GTimeVal start_time;
		g_get_current_time(&start_time);
		active = StartNewSegment(&start_time, "Title", error_r);
		if (!active) {
			return 0;
		}
		g_debug("Segmenter::WriteData segment %u started, window_top=%u",
			active->seq, window_top);
	}
	
	active->Encode(encoder, chunk, size, error_r);
	active->FlushEncodedData(encoder, error_r);
	
	if (!UpdateWindowAndIndexFile(false, error_r))
		return 0;
	return size;
}

static size_t
hls_output_play(struct audio_output *ao, const void *chunk, size_t size,
	GError **error_r)
{
	HlsSegmenter *hls = Cast(ao);
	return hls->Play(chunk, size, error_r);
}

inline bool
HlsSegmenter::Pause()
{
	g_debug("Segmenter::Pause");
	Stop();
	return true;
}

static bool
hls_output_pause(struct audio_output *ao)
{
	HlsSegmenter *hls = Cast(ao);
	return hls->Pause();
}

inline void
HlsSegmenter::SendTag(G_GNUC_UNUSED const struct tag *tag)
{
	g_debug("Segmenter::SendTag");
	assert(tag != nullptr);
	/*
	 * TODO: add tag to metadata, then use WebVTT to 
	 * expose metadata in playlist.
	 */
}

static void
hls_output_tag(struct audio_output *ao, const struct tag *tag)
{
	HlsSegmenter *hls = Cast(ao);
	hls->SendTag(tag);
}

inline void
HlsSegmenter::Cancel()
{
	g_debug("Segmenter::Cancel");
	Stop();
}

static void
hls_output_cancel(struct audio_output *ao)
{
	HlsSegmenter *hls = Cast(ao);	
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
