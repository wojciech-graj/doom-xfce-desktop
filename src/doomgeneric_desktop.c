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

static GDateTime *dt_start;

static unsigned icon_res = 64;
static unsigned iconsx;
static unsigned iconsy;

static GFile *config_file;
static GFile *config_bak_file;

static struct File *files;

static void cleanup(void);
static void handle_signal(int sig);

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

	g_file_copy(config_bak_file, config_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
	g_object_unref(config_file);
	g_object_unref(config_bak_file);

	g_date_time_unref(dt_start);

	system("pkill xfdesktop && xfdesktop &");
}

void DG_Init()
{
	iconsx = (DOOMGENERIC_RESX + icon_res - 1) / icon_res;
	iconsy = (DOOMGENERIC_RESY + icon_res - 1) / icon_res;

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

	unsigned x, y;
	for (y = 0; y < iconsy; y++) {
		for (x = 0; x < iconsx; x++) {
			struct File *file = &files[y * iconsx + x];
			g_autofree char *basename = g_strdup_printf("%03u_%03u.ppm", x, y);
			file->name = g_build_filename(desktop_dir, basename, NULL);
			file->stream = g_fopen(file->name, "w");

			g_key_file_set_integer(key_file, file->name, "row", y);
			g_key_file_set_integer(key_file, file->name, "col", x + 3);
		}
	}

	g_key_file_save_to_file(key_file, config_fname, NULL);

	dt_start = g_date_time_new_now_utc();

	system("pkill xfdesktop && xfdesktop &");
	atexit(&cleanup);
	signal(SIGINT, &handle_signal);
}

void DG_DrawFrame()
{

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
	return 0;
}

void DG_SetWindowTitle(const char *title)
{
	(void)title;
}
