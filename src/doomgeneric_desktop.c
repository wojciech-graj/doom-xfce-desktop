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
//     Nil
//

#include "doomkeys.h"
#include "i_system.h"
#include "doomgeneric.h"

#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#define UNLIKELY(x_) __builtin_expect((x_),0)
#define CALL(stmt_, ...) do {if (UNLIKELY(stmt_)) I_Error(__VA_ARGS__);} while (0)

struct File {
	gchar *name;
	FILE *stream;
};

struct Color {
	uint32_t b:8;
	uint32_t g:8;
	uint32_t r:8;
	uint32_t a:8;
};

struct Key {
	const char *name;
	const unsigned char doomKey;
	const unsigned col;
	const unsigned row;
	gboolean pressed;
};

static GDateTime *dt_start;

static unsigned icon_res = 32;
static unsigned iconsx;
static unsigned iconsy;
static unsigned header_len;
static gchar *img_buffer;

static GFile *config_file;
static GFile *config_bak_file;

GArray *input_backlog;

static struct File *files;

static GFile *input_file;
static gchar *input_fname;

static void cleanup(void);
static void handle_signal(int sig);

#define N_KEYS 6
static struct Key keys[N_KEYS] = {
	{
		.name = "FORWARD.sh",
		.doomKey = KEY_UPARROW,
		.col = 1,
		.row = 0,
	},
	{
		.name = "LEFT.sh",
		.doomKey = KEY_LEFTARROW,
		.col = 0,
		.row = 1,
	},
	{
		.name = "DOWN.sh",
		.doomKey = KEY_DOWNARROW,
		.col = 1,
		.row = 1,
	},
	{
		.name = "RIGHT.sh",
		.doomKey = KEY_RIGHTARROW,
		.col = 2,
		.row = 1,
	},
	{
		.name = "FIRE.sh",
		.doomKey = KEY_FIRE,
		.col = 3,
		.row = 0,
	},
	{
		.name = "USE.sh",
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
	unsigned i;
	for (i = 0; i < iconsx * iconsy; i++) {
		g_autoptr(GFile) file = g_file_new_for_path(files[i].name);
		fclose(files[i].stream);
		g_file_delete(file, NULL, NULL);
		g_free(files[i].name);
	}
	g_free(files);

	const gchar *desktop_dir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
	for (i = 0; i < N_KEYS; i++) {
		const struct Key *key = &keys[i];
		g_autofree gchar *fname = g_build_filename(desktop_dir, key->name, NULL);
		g_autoptr(GFile) file = g_file_new_for_path(fname);
		g_file_delete(file, NULL, NULL);
	}

	g_file_copy(config_bak_file, config_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
	g_object_unref(config_file);
	g_object_unref(config_bak_file);

	g_date_time_unref(dt_start);

	g_free(img_buffer);

	g_object_unref(input_file);
	g_free(input_fname);

	g_array_free(input_backlog, TRUE);

	system("pkill xfdesktop && xfdesktop &");
}

void DG_Init()
{
	iconsx = (DOOMGENERIC_RESX + icon_res - 1) / icon_res;
	iconsy = (DOOMGENERIC_RESY + icon_res - 1) / icon_res;

	input_backlog = g_array_new(FALSE, FALSE, 1);

	const gchar *config_dir = g_get_user_config_dir();
	g_autofree gchar *config_fname_temp = g_build_filename(config_dir, "xfce4/desktop/icons.screen.latest.rc", NULL);
	if (!g_file_test(config_fname_temp, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
		I_Error("Failed to locate file '%s'.", config_fname_temp);

	config_file = g_file_new_for_path(config_fname_temp);
	g_autoptr(GFileInfo) info = NULL;
	gchar *config_fname = config_fname_temp;
	if (g_file_query_file_type(config_file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_SYMBOLIC_LINK) {
		info  = g_file_query_info(config_file, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET, G_FILE_QUERY_INFO_NONE, NULL, NULL);
		g_free(config_fname_temp);
		config_fname_temp = NULL;
		config_fname = (gchar*)g_file_info_get_symlink_target(info);
		g_object_unref(config_file);
		config_file = g_file_new_for_path(config_fname);
	}

	g_autofree gchar *config_bak_fname = g_build_filename(config_dir, "xfce4/desktop/icons.screen.latest.rc.bak", NULL);
	config_bak_file = g_file_new_for_path(config_bak_fname);

	g_file_copy(config_file, config_bak_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);

	g_autoptr(GKeyFile) key_file = g_key_file_new();
	g_key_file_load_from_file(key_file, config_fname, G_KEY_FILE_NONE, NULL);

	files = g_malloc(iconsx * iconsy * sizeof(struct File));
	const gchar *desktop_dir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
	g_autofree gchar *header = g_strdup_printf("P6\n%u %u\n255\n", icon_res, icon_res);
	header_len = strlen(header);

	unsigned x, y;
	for (y = 0; y < iconsy; y++) {
		for (x = 0; x < iconsx; x++) {
			struct File *file = &files[y * iconsx + x];
			g_autofree gchar *basename = g_strdup_printf("%03u_%03u.ppm", x, y);
			file->name = g_build_filename(desktop_dir, basename, NULL);
			file->stream = g_fopen(file->name, "w");
			fwrite(header, 1, header_len, file->stream);

			g_key_file_set_integer(key_file, file->name, "row", y);
			g_key_file_set_integer(key_file, file->name, "col", x + 3);
		}
	}

	g_autoptr(GFileIOStream) input_iostream;
	input_file = g_file_new_tmp(NULL, &input_iostream, NULL);
	input_fname = g_file_get_path(input_file);

	unsigned i;
	for (i = 0; i < N_KEYS; i++) {
		const struct Key *key = &keys[i];
		g_autofree gchar *fname = g_build_filename(desktop_dir, key->name, NULL);
		FILE *file = g_fopen(fname, "w");
		g_autofree gchar *script = g_strdup_printf("echo \"%u \" >> \"%s\"", i, input_fname);
		fwrite(script, 1, strlen(script), file);
		fclose(file);
		g_chmod(fname, S_IRWXU | S_IRWXG | S_IRWXO);

		g_key_file_set_integer(key_file, fname, "row", iconsy + key->row);
		g_key_file_set_integer(key_file, fname, "col", 3 + key->col);
	}

	g_key_file_save_to_file(key_file, config_fname, NULL);
	system("pkill xfdesktop && xfdesktop &");

	img_buffer = g_malloc(icon_res * icon_res * 3);

	dt_start = g_date_time_new_now_utc();

	atexit(&cleanup);
	signal(SIGINT, &handle_signal);
}

void DG_DrawFrame()
{
	struct Color *pixels = (struct Color*)DG_ScreenBuffer;

	unsigned x, y;
	for (y = 0; y < iconsy; y++) {
		for (x = 0; x < iconsx; x++) {
			struct File *file = &files[y * iconsx + x];
			fseek(file->stream, header_len, SEEK_SET);

			memset(img_buffer, '\0', icon_res * icon_res * 3);
			unsigned imgx, imgy;
			unsigned imgi = 0;
			for (imgy = y * icon_res; imgy < MIN(y * icon_res + icon_res, DOOMGENERIC_RESY); imgy++) {
				for (imgx = x * icon_res; imgx < MIN(x * icon_res + icon_res, DOOMGENERIC_RESX); imgx++) {
					struct Color pix = pixels[imgy * DOOMGENERIC_RESX + imgx];
					img_buffer[imgi++] = pix.r;
					img_buffer[imgi++] = pix.g;
					img_buffer[imgi++] = pix.b;
				}
			}

			fwrite(img_buffer, 3, icon_res * icon_res, file->stream);
			fflush(file->stream);
		}
	}
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

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
	if (input_backlog->len) {
	DG_GetKey_GOT_INPUT:
		{
		unsigned char keyi = g_array_index(input_backlog, unsigned char, input_backlog->len - 1);
		g_array_remove_index(input_backlog, input_backlog->len - 1);
		struct Key *key = &keys[keyi];
		key->pressed = !key->pressed;
		*pressed = key->pressed;
		*doomKey = key->doomKey;
		return 1;
		}
	}
	FILE *file = fopen(input_fname, "r");
	unsigned char keyi;
	while (fscanf(file, "%hhu", &keyi) != EOF)
		g_array_append_val(input_backlog, keyi);
	fclose(file);
	if (input_backlog->len) {
		file = fopen(input_fname, "w");
		fclose(file);
		puts("HERE");
		goto DG_GetKey_GOT_INPUT;
	}
	return 0;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}
