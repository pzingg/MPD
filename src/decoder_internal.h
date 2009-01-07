/* the Music Player Daemon (MPD)
 * Copyright (C) 2008 Max Kellermann <max@duempel.org>
 * This project's homepage is: http://www.musicpd.org
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MPD_DECODER_INTERNAL_H
#define MPD_DECODER_INTERNAL_H

#include "decoder_api.h"
#include "pcm_utils.h"

struct decoder {
	struct pcm_convert_state conv_state;

	bool seeking;

	/** the last tag received from the stream */
	struct tag *stream_tag;

	/** the last tag received from the decoder plugin */
	struct tag *decoder_tag;
};

#endif