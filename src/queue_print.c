/*
 * Copyright (C) 2003-2009 The Music Player Daemon Project
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "queue_print.h"
#include "queue.h"
#include "song.h"
#include "song_print.h"
#include "client.h"

void
queue_print_song_info(struct client *client, const struct queue *queue,
		      unsigned position)
{
	song_print_info(client, queue_get(queue, position));
	client_printf(client, "Pos: %u\nId: %u\n",
		      position, queue_position_to_id(queue, position));
}

void
queue_print_info(struct client *client, const struct queue *queue,
		 unsigned start, unsigned end)
{
	assert(start <= end);
	assert(end <= queue_length(queue));

	for (unsigned i = start; i < end; ++i)
		queue_print_song_info(client, queue, i);
}

void
queue_print_uris(struct client *client, const struct queue *queue,
		 unsigned start, unsigned end)
{
	assert(start <= end);
	assert(end <= queue_length(queue));

	for (unsigned i = start; i < end; ++i) {
		const struct song *song = queue_get(queue, i);
		char *uri = song_get_uri(song);

		client_printf(client, "%i:%s\n", i, uri);
		g_free(uri);
	}
}

void
queue_print_changes_info(struct client *client, const struct queue *queue,
			 uint32_t version)
{
	for (unsigned i = 0; i < queue_length(queue); i++) {
		if (queue_song_newer(queue, i, version))
			queue_print_song_info(client, queue, i);
	}
}

void
queue_print_changes_position(struct client *client, const struct queue *queue,
			     uint32_t version)
{
	for (unsigned i = 0; i < queue_length(queue); i++)
		if (queue_song_newer(queue, i, version))
			client_printf(client, "cpos: %i\nId: %i\n",
				      i, queue_position_to_id(queue, i));
}