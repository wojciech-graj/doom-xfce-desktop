//
// Copyright(C) 2023 Wojciech Graj
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//     DooM for the xfce4 desktop
//

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_system.h"
#include "m_argv.h"

#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <signal.h>
#include <stdlib.h>

#define xfce_restart(void)                                \
	do {                                              \
		system("pkill xfdesktop && xfdesktop &"); \
	} while (0)

#define CALL_ERRNO(invoc, cond)                                          \
	({                                                               \
		typeof(invoc) res = (invoc);                             \
		if (G_UNLIKELY(res cond)) {                              \
			I_Error("Error %d: %s", errno, strerror(errno)); \
		}                                                        \
		res;                                                     \
	})

#define CALL_MSG(invoc, cond, msg)                 \
	({                                         \
		typeof(invoc) res = (invoc);       \
		if (G_UNLIKELY(res cond)) {        \
			I_Error("Error: %s", msg); \
		}                                  \
		res;                               \
	})

#define CALL_GERROR(func, ...)                                                         \
	({                                                                             \
		GError *error = NULL;                                                  \
		typeof((func)(__VA_ARGS__, &error)) res = (func)(__VA_ARGS__, &error); \
		if (G_UNLIKELY(error)) {                                               \
			I_Error("Error: %s\n", error->message);                        \
			g_error_free(error);                                           \
		}                                                                      \
		res;                                                                   \
	})

struct Color {
	guint32 b : 8;
	guint32 g : 8;
	guint32 r : 8;
	guint32 a : 8;
};

struct Key {
	const gchar *name;
	const unsigned char doomKey;
	const guint col;
	const guint row;
	gboolean pressed;
};

static GDateTime *dt_start;

static guint icon_res = 64;
static guint iconsx;
static guint iconsy;
static guint header_len;
static gchar *img_buffer;

static GFile *config_file;
static GFile *config_bak_file;

static GArray *input_backlog;

static gchar **fnames;
static guint n_files;

static GFile *input_file;
static gchar *input_fname;

static guint frame_delay = 400;

static void cleanup(void);
static void handle_signal(int sig);

static struct Key keys[] = {
	{
		.name = "FORWARD",
		.doomKey = KEY_UPARROW,
		.col = 1,
		.row = 0,
	},
	{
		.name = "LEFT",
		.doomKey = KEY_LEFTARROW,
		.col = 0,
		.row = 1,
	},
	{
		.name = "DOWN",
		.doomKey = KEY_DOWNARROW,
		.col = 1,
		.row = 1,
	},
	{
		.name = "RIGHT",
		.doomKey = KEY_RIGHTARROW,
		.col = 2,
		.row = 1,
	},
	{
		.name = "FIRE",
		.doomKey = KEY_FIRE,
		.col = 3,
		.row = 0,
	},
	{
		.name = "USE",
		.doomKey = KEY_USE,
		.col = 3,
		.row = 1,
	},
};

void handle_signal(int sig)
{
	exit(1);
	(void)sig;
}

void cleanup(void)
{
	/* Delete files */
	guint i;
	for (i = 0; i < iconsx * iconsy; i++) {
		g_autoptr(GFile) file = g_file_new_for_path(fnames[i]);
		g_file_delete(file, NULL, NULL);
	}
	for (; i < n_files; i++) {
		g_autoptr(GFile) file = g_file_new_for_path(fnames[i]);
		g_file_delete(file, NULL, NULL);
		g_autofree gchar *active_fname = g_strconcat(fnames[i], "(ACTIVE)", NULL);
		g_autoptr(GFile) active_file = g_file_new_for_path(active_fname);
		g_file_delete(active_file, NULL, NULL);
	}

	/* Restore backup desktop config */
	xfce_restart();
	usleep(G_USEC_PER_SEC);
	g_file_copy(config_bak_file, config_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
	xfce_restart();
	g_file_delete(config_bak_file, NULL, NULL);

	/* Free resources */
	g_strfreev(fnames);
	g_object_unref(config_file);
	g_object_unref(config_bak_file);
	g_date_time_unref(dt_start);
	g_free(img_buffer);
	g_object_unref(input_file);
	g_free(input_fname);
	g_array_free(input_backlog, TRUE);
}

void DG_Init()
{
	/* Parse args */
	int argi = M_CheckParmWithArgs("-res", 1);
	if (argi > 0)
		icon_res = atoi(myargv[argi + 1]);
	argi = M_CheckParmWithArgs("-delay", 1);
	if (argi > 0)
		frame_delay = atoi(myargv[argi + 1]);

	/* Initialize image */
	iconsx = (DOOMGENERIC_RESX + icon_res - 1) / icon_res;
	iconsy = (DOOMGENERIC_RESY + icon_res - 1) / icon_res;
	img_buffer = g_malloc(icon_res * icon_res * 3);

	/* Initialize input */
	input_backlog = g_array_new(FALSE, FALSE, 1);
	g_autoptr(GFileIOStream) input_iostream;
	input_file = CALL_GERROR(g_file_new_tmp, NULL, &input_iostream);
	input_fname = CALL_MSG(g_file_get_path(input_file), == NULL, "Failed to get path of input file.");

	/* Verify desktop config exists */
	const gchar *config_dir = g_get_user_config_dir();
	g_autofree gchar *config_fname_temp = g_build_filename(config_dir, "xfce4/desktop/icons.screen.latest.rc", NULL);
	if (!g_file_test(config_fname_temp, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
		I_Error("Failed to locate file '%s'.", config_fname_temp);

	/* Follow desktop config symlinks */
	config_file = g_file_new_for_path(config_fname_temp);
	g_autoptr(GFileInfo) info = NULL;
	gchar *config_fname = config_fname_temp;
	if (g_file_query_file_type(config_file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK) {
		info = CALL_GERROR(g_file_query_info, config_file, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET, G_FILE_QUERY_INFO_NONE, NULL);
		g_free(config_fname_temp);
		config_fname_temp = NULL;
		config_fname = (gchar *)g_file_info_get_symlink_target(info);
		g_object_unref(config_file);
		config_file = g_file_new_for_path(config_fname);
	}

	/* Backup desktop config */
	g_autofree gchar *config_bak_fname = g_build_filename(config_dir, "xfce4/desktop/icons.screen.latest.rc.bak", NULL);
	config_bak_file = g_file_new_for_path(config_bak_fname);
	CALL_GERROR(g_file_copy, config_file, config_bak_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL);

	/* Load desktop config */
	g_autoptr(GKeyFile) key_file = g_key_file_new();
	CALL_GERROR(g_key_file_load_from_file, key_file, config_fname, G_KEY_FILE_NONE);

	/* Free up desktop space for the game */
	gchar **groups = g_key_file_get_groups(key_file, NULL);
	guint i;
	gchar **group;
	for (group = groups + 1; *group; group++) {
		guint col = CALL_GERROR(g_key_file_get_integer, key_file, *group, "col");
		if (col < iconsx + 1)
			g_key_file_set_integer(key_file, *group, "col", col + iconsx + 1);
	}
	g_strfreev(groups);

	/* Create desktop display files */
	n_files = iconsx * iconsy + G_N_ELEMENTS(keys);
	fnames = g_malloc0((n_files + 1) * sizeof(char *));
	const gchar *desktop_dir = CALL_MSG(g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP), == NULL, "Failed to get desktop directory.");
	g_autofree gchar *header = g_strdup_printf("P6\n%u %u\n255\n", icon_res, icon_res);
	header_len = strlen(header);

	guint x, y, fi = 0;
	for (y = 0; y < iconsy; y++) {
		for (x = 0; x < iconsx; x++) {
			char **fname = &fnames[fi++];
			g_autofree gchar *basename = g_strdup_printf("%c%c.ppm", x + 'a', y + 'a');
			*fname = g_build_filename(desktop_dir, basename, NULL);
			FILE *f = CALL_ERRNO(g_fopen(*fname, "w"), == NULL);
			CALL_ERRNO(fwrite(header, 1, header_len, f), != header_len);
			CALL_ERRNO(fclose(f), == EOF);

			g_key_file_set_integer(key_file, *fname, "row", y);
			g_key_file_set_integer(key_file, *fname, "col", x);
		}
	}

	/* Create desktop controls files */
	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		const struct Key *key = &keys[i];
		gchar *fname = g_build_filename(desktop_dir, key->name, NULL);
		FILE *file = CALL_ERRNO(g_fopen(fname, "w"), == NULL);
		g_autofree gchar *script = g_strdup_printf("#!/bin/bash\necho \"%u \" >> \"%s\"", i, input_fname);
		CALL_ERRNO(fwrite(script, 1, strlen(script), file), != strlen(script));
		CALL_ERRNO(fclose(file), == EOF);
		CALL_ERRNO(g_chmod(fname, S_IRWXU | S_IRWXG | S_IRWXO), == -1);
		fnames[fi++] = fname;

		g_key_file_set_integer(key_file, fname, "row", iconsy + key->row);
		g_key_file_set_integer(key_file, fname, "col", key->col);
	}

	/* Apply changes to desktop config */
	CALL_GERROR(g_key_file_save_to_file, key_file, config_fname);
	xfce_restart();

	dt_start = g_date_time_new_now_utc();
	CALL_ERRNO(atexit(&cleanup), != 0);
	CALL_ERRNO(signal(SIGINT, &handle_signal), == SIG_ERR);
}

void DG_DrawFrame()
{
	struct Color *pixels = (struct Color *)DG_ScreenBuffer;

	guint x, y;
	for (y = 0; y < iconsy; y++) {
		for (x = 0; x < iconsx; x++) {
			FILE *f = CALL_ERRNO(g_fopen(fnames[y * iconsx + x], "r+"), == NULL);
			CALL_ERRNO(fseek(f, header_len, SEEK_SET), == -1);

			memset(img_buffer, '\0', icon_res * icon_res * 3);
			guint imgx, imgy;
			for (imgy = 0; imgy < MIN(icon_res, DOOMGENERIC_RESY - y * icon_res); imgy++) {
				for (imgx = 0; imgx < MIN(icon_res, DOOMGENERIC_RESX - x * icon_res); imgx++) {
					struct Color pix = pixels[(y * icon_res + imgy) * DOOMGENERIC_RESX + (x * icon_res + imgx)];
					guint imgi = (imgy * icon_res + imgx) * 3;
					img_buffer[imgi] = pix.r;
					img_buffer[imgi + 1] = pix.g;
					img_buffer[imgi + 2] = pix.b;
				}
			}

			CALL_ERRNO(fwrite(img_buffer, 3, icon_res * icon_res, f), != 3 * icon_res * icon_res);
			fclose(f);
		}
	}

	g_usleep(frame_delay * 1000UL);
}

void DG_SleepMs(uint32_t ms)
{
	g_usleep(ms * 1000UL);
}

uint32_t DG_GetTicksMs()
{
	GDateTime *dt_now = g_date_time_new_now_utc();
	GTimeSpan diff = g_date_time_difference(dt_now, dt_start);
	g_date_time_unref(dt_now);
	return diff / 1000LL;
}

int DG_GetKey(int *pressed, unsigned char *doomKey)
{
	if (input_backlog->len) {
	DG_GetKey_GOT_INPUT : {
		unsigned char keyi = g_array_index(input_backlog, unsigned char, input_backlog->len - 1);
		g_array_remove_index(input_backlog, input_backlog->len - 1);
		struct Key *key = &keys[keyi];
		key->pressed = !key->pressed;
		*pressed = key->pressed;
		*doomKey = key->doomKey;
		gchar *orig_fname = fnames[iconsx * iconsy + keyi];
		g_autofree gchar *active_fname = g_strconcat(orig_fname, "(ACTIVE)", NULL);
		CALL_ERRNO(g_rename(key->pressed ? orig_fname : active_fname, key->pressed ? active_fname : orig_fname), == -1);
		return 1;
	}
	}

	/* Read inputs from input file into backlog */
	FILE *file = CALL_ERRNO(g_fopen(input_fname, "r"), == NULL);
	unsigned char keyi;
	while (fscanf(file, "%hhu", &keyi) != EOF)
		g_array_append_val(input_backlog, keyi);
	CALL_ERRNO(fclose(file), == EOF);

	if (input_backlog->len) {
		file = CALL_ERRNO(g_fopen(input_fname, "w"), == NULL);
		CALL_ERRNO(fclose(file), == EOF);
		goto DG_GetKey_GOT_INPUT;
	}
	return 0;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}
