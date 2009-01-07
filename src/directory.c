/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
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

#include "directory.h"
#include "song.h"
#include "path.h"

#include <glib.h>

#include <assert.h>
#include <string.h>
#include <stdlib.h>

struct directory *
directory_new(const char *path, struct directory *parent)
{
	struct directory *directory;
	size_t pathlen = strlen(path);

	assert(path != NULL);
	assert((*path == 0) == (parent == NULL));

	directory = g_malloc0(sizeof(*directory) -
			      sizeof(directory->path) + pathlen + 1);
	directory->parent = parent;
	memcpy(directory->path, path, pathlen + 1);

	return directory;
}

void
directory_free(struct directory *directory)
{
	for (unsigned i = 0; i < directory->songs.nr; ++i)
		song_free(directory->songs.base[i]);

	for (unsigned i = 0; i < directory->children.nr; ++i)
		directory_free(directory->children.base[i]);

	dirvec_destroy(&directory->children);
	songvec_destroy(&directory->songs);
	free(directory);
	/* this resets last dir returned */
	/*directory_get_path(NULL); */
}

const char *
directory_get_name(const struct directory *directory)
{
	return g_basename(directory->path);
}

void
directory_prune_empty(struct directory *directory)
{
	int i;
	struct dirvec *dv = &directory->children;

	for (i = dv->nr; --i >= 0; ) {
		directory_prune_empty(dv->base[i]);
		if (directory_is_empty(dv->base[i]))
			dirvec_delete(dv, dv->base[i]);
	}
	if (!dv->nr)
		dirvec_destroy(dv);
}

struct directory *
directory_get_directory(struct directory *directory, const char *name)
{
	struct directory *cur = directory;
	struct directory *found = NULL;
	char *duplicated;
	char *locate;

	assert(name != NULL);

	if (isRootDirectory(name))
		return directory;

	duplicated = g_strdup(name);
	locate = strchr(duplicated, '/');
	while (1) {
		if (locate)
			*locate = '\0';
		if (!(found = directory_get_child(cur, duplicated)))
			break;
		assert(cur == found->parent);
		cur = found;
		if (!locate)
			break;
		*locate = '/';
		locate = strchr(locate + 1, '/');
	}

	free(duplicated);

	return found;
}

void
directory_sort(struct directory *directory)
{
	int i;
	struct dirvec *dv = &directory->children;

	dirvec_sort(dv);
	songvec_sort(&directory->songs);

	for (i = dv->nr; --i >= 0; )
		directory_sort(dv->base[i]);
}

int
directory_walk(struct directory *directory,
	       int (*forEachSong)(struct song *, void *),
	       int (*forEachDir)(struct directory *, void *),
	       void *data)
{
	struct dirvec *dv = &directory->children;
	int err = 0;
	size_t j;

	if (forEachDir && (err = forEachDir(directory, data)) < 0)
		return err;

	if (forEachSong) {
		err = songvec_for_each(&directory->songs, forEachSong, data);
		if (err < 0)
			return err;
	}

	for (j = 0; err >= 0 && j < dv->nr; ++j)
		err = directory_walk(dv->base[j], forEachSong,
						forEachDir, data);

	return err;
}