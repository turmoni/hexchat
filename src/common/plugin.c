/* X-Chat
 * Copyright (C) 2002 Peter Zelezny.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "xchat.h"
#include "fe.h"
#include "util.h"
#include "outbound.h"
#include "cfgfiles.h"
#include "ignore.h"
#include "server.h"
#include "servlist.h"
#include "modes.h"
#include "notify.h"
#include "text.h"
#define PLUGIN_C
typedef struct session xchat_context;
#include "xchat-plugin.h"
#include "plugin.h"


#include "xchatc.h"

/* the USE_PLUGIN define only removes libdl stuff */

#ifdef USE_PLUGIN
#ifdef USE_GMODULE
#include <gmodule.h>
#else
#include <dlfcn.h>
#endif
#endif

#define DEBUG(x) {x;}

/* crafted to be an even 32 bytes */
struct _xchat_hook
{
	xchat_plugin *pl;	/* the plugin to which it belongs */
	char *name;			/* "xdcc" */
	void *callback;	/* pointer to xdcc_callback */
	char *help_text;	/* help_text for commands only */
	void *userdata;	/* passed to the callback */
	int tag;				/* for timers & FDs only */
	int type;			/* HOOK_* */
	int pri;	/* fd */	/* priority / fd for HOOK_FD only */
};

struct _xchat_list
{
	int type;			/* LIST_* */
	GSList *pos;		/* current pos */
	GSList *next;		/* next pos */
	GSList *head;		/* for LIST_USERS only */
	struct notify_per_server *notifyps;	/* notify_per_server * */
};

typedef int (xchat_cmd_cb) (char *word[], char *word_eol[], void *user_data);
typedef int (xchat_serv_cb) (char *word[], char *word_eol[], void *user_data);
typedef int (xchat_print_cb) (char *word[], void *user_data);
typedef int (xchat_fd_cb) (int fd, int flags, void *user_data);
typedef int (xchat_timer_cb) (void *user_data);
typedef int (xchat_init_func) (xchat_plugin *, char **, char **, char **, char *);
typedef int (xchat_deinit_func) (xchat_plugin *);

enum
{
	LIST_CHANNELS,
	LIST_DCC,
	LIST_IGNORE,
	LIST_NOTIFY,
	LIST_USERS
};

enum
{
	HOOK_COMMAND,	/* /command */
	HOOK_SERVER,	/* PRIVMSG, NOTICE, numerics */
	HOOK_PRINT,		/* All print events */
	HOOK_TIMER,		/* timeouts */
	HOOK_FD,			/* sockets & fds */
	HOOK_DELETED	/* marked for deletion */
};

GSList *plugin_list = NULL;	/* export for plugingui.c */
static GSList *hook_list = NULL;

extern const struct prefs vars[];	/* cfgfiles.c */


/* unload a plugin and remove it from our linked list */

static int
plugin_free (xchat_plugin *pl, int do_deinit, int allow_refuse)
{
	GSList *list, *next;
	xchat_hook *hook;
	xchat_deinit_func *deinit_func;

	/* fake plugin added by xchat_plugingui_add() */
	if (pl->fake)
		goto xit;

	/* run the plugin's deinit routine, if any */
	if (do_deinit && pl->deinit_callback != NULL)
	{
		deinit_func = pl->deinit_callback;
		if (!deinit_func (pl) && allow_refuse)
			return FALSE;
	}

	/* remove all of this plugin's hooks */
	list = hook_list;
	while (list)
	{
		hook = list->data;
		next = list->next;
		if (hook->pl == pl)
			xchat_unhook (NULL, hook);
		list = next;
	}

#ifdef USE_PLUGIN
	if (pl->handle)
#ifdef USE_GMODULE
		g_module_close (pl->handle);
#else
		dlclose (pl->handle);
#endif
#endif

xit:
	if (pl->free_strings)
	{
		if (pl->name)
			free (pl->name);
		if (pl->desc)
			free (pl->desc);
		if (pl->version)
			free (pl->version);
	}
	if (pl->filename)
		free ((char *)pl->filename);
	free (pl);

	plugin_list = g_slist_remove (plugin_list, pl);

#ifdef USE_PLUGIN
	fe_pluginlist_update ();
#endif

	return TRUE;
}

static xchat_plugin *
plugin_list_add (xchat_context *ctx, char *filename, const char *name,
					  const char *desc, const char *version, void *handle,
					  void *deinit_func, int fake, int free_strings)
{
	xchat_plugin *pl;

	pl = malloc (sizeof (xchat_plugin));
	pl->handle = handle;
	pl->filename = filename;
	pl->context = ctx;
	pl->name = (char *)name;
	pl->desc = (char *)desc;
	pl->version = (char *)version;
	pl->deinit_callback = deinit_func;
	pl->fake = fake;
	pl->free_strings = free_strings;	/* free() name,desc,version? */

	plugin_list = g_slist_prepend (plugin_list, pl);

	return pl;
}

static void *
xchat_dummy (xchat_plugin *ph)
{
	return NULL;
}

#ifdef WIN32
static int
xchat_read_fd (xchat_plugin *ph, GIOChannel *source, char *buf, int *len)
{
	GError *error = NULL;

	g_io_channel_set_buffered (source, FALSE);
	g_io_channel_set_encoding (source, NULL, &error);

	if (g_io_channel_read_chars (source, buf, *len, (gsize*)len, &error) == G_IO_STATUS_NORMAL)
	{
		return 0;
	}
	else
	{
		return -1;
	}
}
#endif

/* Load a static plugin */

void
plugin_add (session *sess, char *filename, void *handle, void *init_func,
				void *deinit_func, char *arg, int fake)
{
	xchat_plugin *pl;
	char *file;

	file = NULL;
	if (filename)
		file = strdup (filename);

	pl = plugin_list_add (sess, file, file, NULL, NULL, handle, deinit_func,
								 fake, FALSE);

	if (!fake)
	{
		/* win32 uses these because it doesn't have --export-dynamic! */
		pl->xchat_hook_command = xchat_hook_command;
		pl->xchat_hook_server = xchat_hook_server;
		pl->xchat_hook_print = xchat_hook_print;
		pl->xchat_hook_timer = xchat_hook_timer;
		pl->xchat_hook_fd = xchat_hook_fd;
		pl->xchat_unhook = xchat_unhook;
		pl->xchat_print = xchat_print;
		pl->xchat_printf = xchat_printf;
		pl->xchat_command = xchat_command;
		pl->xchat_commandf = xchat_commandf;
		pl->xchat_nickcmp = xchat_nickcmp;
		pl->xchat_set_context = xchat_set_context;
		pl->xchat_find_context = xchat_find_context;
		pl->xchat_get_context = xchat_get_context;
		pl->xchat_get_info = xchat_get_info;
		pl->xchat_get_prefs = xchat_get_prefs;
		pl->xchat_list_get = xchat_list_get;
		pl->xchat_list_free = xchat_list_free;
		pl->xchat_list_fields = xchat_list_fields;
		pl->xchat_list_str = xchat_list_str;
		pl->xchat_list_next = xchat_list_next;
		pl->xchat_list_int = xchat_list_int;
		pl->xchat_plugingui_add = xchat_plugingui_add;
		pl->xchat_plugingui_remove = xchat_plugingui_remove;
		pl->xchat_emit_print = xchat_emit_print;
#ifdef WIN32
		pl->xchat_read_fd = (void *) xchat_read_fd;
#else
		pl->xchat_read_fd = xchat_dummy;
#endif
		pl->xchat_list_time = xchat_list_time;
		pl->xchat_gettext = xchat_gettext;
		pl->xchat_send_modes = xchat_send_modes;
		pl->xchat_strip = xchat_strip;
		pl->xchat_free = xchat_free;
		pl->xchat_pluginpref_set_str = xchat_pluginpref_set_str;
		pl->xchat_pluginpref_get_str = xchat_pluginpref_get_str;
		pl->xchat_pluginpref_set_int = xchat_pluginpref_set_int;
		pl->xchat_pluginpref_get_int = xchat_pluginpref_get_int;
		pl->xchat_pluginpref_delete = xchat_pluginpref_delete;
		pl->xchat_pluginpref_list = xchat_pluginpref_list;

		/* incase new plugins are loaded on older xchat */
		pl->xchat_dummy4 = xchat_dummy;
		pl->xchat_dummy3 = xchat_dummy;
		pl->xchat_dummy2 = xchat_dummy;
		pl->xchat_dummy1 = xchat_dummy;

		/* run xchat_plugin_init, if it returns 0, close the plugin */
		if (((xchat_init_func *)init_func) (pl, &pl->name, &pl->desc, &pl->version, arg) == 0)
		{
			plugin_free (pl, FALSE, FALSE);
			return;
		}
	}

#ifdef USE_PLUGIN
	fe_pluginlist_update ();
#endif
}

/* kill any plugin by the given (file) name (used by /unload) */

int
plugin_kill (char *name, int by_filename)
{
	GSList *list;
	xchat_plugin *pl;

	list = plugin_list;
	while (list)
	{
		pl = list->data;
		/* static-plugins (plugin-timer.c) have a NULL filename */
		if ((by_filename && pl->filename && g_ascii_strcasecmp (name, pl->filename) == 0) ||
			 (by_filename && pl->filename && g_ascii_strcasecmp (name, file_part (pl->filename)) == 0) ||
			(!by_filename && g_ascii_strcasecmp (name, pl->name) == 0))
		{
			/* statically linked plugins have a NULL filename */
			if (pl->filename != NULL && !pl->fake)
			{
				if (plugin_free (pl, TRUE, TRUE))
					return 1;
				return 2;
			}
		}
		list = list->next;
	}

	return 0;
}

/* kill all running plugins (at shutdown) */

void
plugin_kill_all (void)
{
	GSList *list, *next;
	xchat_plugin *pl;

	list = plugin_list;
	while (list)
	{
		pl = list->data;
		next = list->next;
		if (!pl->fake)
			plugin_free (list->data, TRUE, FALSE);
		list = next;
	}
}

#ifdef USE_PLUGIN

/* load a plugin from a filename. Returns: NULL-success or an error string */

char *
plugin_load (session *sess, char *filename, char *arg)
{
	void *handle;
	xchat_init_func *init_func;
	xchat_deinit_func *deinit_func;

#ifdef USE_GMODULE
	/* load the plugin */
	handle = g_module_open (filename, 0);
	if (handle == NULL)
		return (char *)g_module_error ();

	/* find the init routine xchat_plugin_init */
	if (!g_module_symbol (handle, "xchat_plugin_init", (gpointer *)&init_func))
	{
		g_module_close (handle);
		return _("No xchat_plugin_init symbol; is this really an xchat plugin?");
	}

	/* find the plugin's deinit routine, if any */
	if (!g_module_symbol (handle, "xchat_plugin_deinit", (gpointer *)&deinit_func))
		deinit_func = NULL;

#else
	char *error;
	char *filepart;

/* OpenBSD lacks this! */
#ifndef RTLD_GLOBAL
#define RTLD_GLOBAL 0
#endif

#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif

	/* get the filename without path */
	filepart = file_part (filename);

	/* load the plugin */
	if (filepart &&
		 /* xsys draws in libgtk-1.2, causing crashes, so force RTLD_LOCAL */
		 (strstr (filepart, "local") || strncmp (filepart, "libxsys-1", 9) == 0)
		)
		handle = dlopen (filename, RTLD_NOW);
	else
		handle = dlopen (filename, RTLD_GLOBAL | RTLD_NOW);
	if (handle == NULL)
		return (char *)dlerror ();
	dlerror ();		/* Clear any existing error */

	/* find the init routine xchat_plugin_init */
	init_func = dlsym (handle, "xchat_plugin_init");
	error = (char *)dlerror ();
	if (error != NULL)
	{
		dlclose (handle);
		return _("No xchat_plugin_init symbol; is this really an xchat plugin?");
	}

	/* find the plugin's deinit routine, if any */
	deinit_func = dlsym (handle, "xchat_plugin_deinit");
	error = (char *)dlerror ();
#endif

	/* add it to our linked list */
	plugin_add (sess, filename, handle, init_func, deinit_func, arg, FALSE);

	return NULL;
}

static session *ps;

static void
plugin_auto_load_cb (char *filename)
{
	char *pMsg;

#ifndef WIN32	/* black listed */
	if (!strcmp (file_part (filename), "dbus.so"))
		return;
#endif

	pMsg = plugin_load (ps, filename, NULL);
	if (pMsg)
	{
		PrintTextf (ps, "AutoLoad failed for: %s\n", filename);
		PrintText (ps, pMsg);
	}
}

void
plugin_auto_load (session *sess)
{
	/* let's do it the Perl way */
	const char *xdir;
	char *sub_dir;
	ps = sess;

	xdir = get_xdir_fs ();
	sub_dir = malloc (strlen (xdir) + 8);
	strcpy (sub_dir, xdir);
	strcat (sub_dir, "/addons");

#ifdef WIN32
	/* a long list of bundled plugins that should be loaded automatically,
	 * user plugins should go to <config>, leave Program Files alone! */
	for_files ("./plugins", "hcchecksum.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcdns.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcdoat.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcexec.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcfishlim.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hclua.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcmpcinfo.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcperl.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcpython.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcsasl.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hctcl.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcupd.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcwinamp.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcwinsys.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hcwmpa.dll", plugin_auto_load_cb);
	for_files ("./plugins", "hextray.dll", plugin_auto_load_cb);

	for_files (sub_dir, "*.dll", plugin_auto_load_cb);
#else
#if defined(__hpux)
	for_files (HEXCHATLIBDIR"/plugins", "*.sl", plugin_auto_load_cb);
	for_files (sub_dir, "*.sl", plugin_auto_load_cb);
#else
	for_files (HEXCHATLIBDIR"/plugins", "*.so", plugin_auto_load_cb);
	for_files (sub_dir, "*.so", plugin_auto_load_cb);
#endif
#endif

	free (sub_dir);
}

#endif

static GSList *
plugin_hook_find (GSList *list, int type, char *name)
{
	xchat_hook *hook;

	while (list)
	{
		hook = list->data;
		if (hook->type == type)
		{
			if (g_ascii_strcasecmp (hook->name, name) == 0)
				return list;

			if (type == HOOK_SERVER)
			{
				if (g_ascii_strcasecmp (hook->name, "RAW LINE") == 0)
					return list;
			}
		}
		list = list->next;
	}

	return NULL;
}

/* check for plugin hooks and run them */

static int
plugin_hook_run (session *sess, char *name, char *word[], char *word_eol[], int type)
{
	GSList *list, *next;
	xchat_hook *hook;
	int ret, eat = 0;

	list = hook_list;
	while (1)
	{
		list = plugin_hook_find (list, type, name);
		if (!list)
			goto xit;

		hook = list->data;
		next = list->next;
		hook->pl->context = sess;

		/* run the plugin's callback function */
		switch (type)
		{
		case HOOK_COMMAND:
			ret = ((xchat_cmd_cb *)hook->callback) (word, word_eol, hook->userdata);
			break;
		case HOOK_SERVER:
			ret = ((xchat_serv_cb *)hook->callback) (word, word_eol, hook->userdata);
			break;
		default: /*case HOOK_PRINT:*/
			ret = ((xchat_print_cb *)hook->callback) (word, hook->userdata);
			break;
		}

		if ((ret & XCHAT_EAT_XCHAT) && (ret & XCHAT_EAT_PLUGIN))
		{
			eat = 1;
			goto xit;
		}
		if (ret & XCHAT_EAT_PLUGIN)
			goto xit;	/* stop running plugins */
		if (ret & XCHAT_EAT_XCHAT)
			eat = 1;	/* eventually we'll return 1, but continue running plugins */

		list = next;
	}

xit:
	/* really remove deleted hooks now */
	list = hook_list;
	while (list)
	{
		hook = list->data;
		next = list->next;
		if (hook->type == HOOK_DELETED)
		{
			hook_list = g_slist_remove (hook_list, hook);
			free (hook);
		}
		list = next;
	}

	return eat;
}

/* execute a plugged in command. Called from outbound.c */

int
plugin_emit_command (session *sess, char *name, char *word[], char *word_eol[])
{
	return plugin_hook_run (sess, name, word, word_eol, HOOK_COMMAND);
}

/* got a server PRIVMSG, NOTICE, numeric etc... */

int
plugin_emit_server (session *sess, char *name, char *word[], char *word_eol[])
{
	return plugin_hook_run (sess, name, word, word_eol, HOOK_SERVER);
}

/* see if any plugins are interested in this print event */

int
plugin_emit_print (session *sess, char *word[])
{
	return plugin_hook_run (sess, word[0], word, NULL, HOOK_PRINT);
}

int
plugin_emit_dummy_print (session *sess, char *name)
{
	char *word[32];
	int i;

	word[0] = name;
	for (i = 1; i < 32; i++)
		word[i] = "\000";

	return plugin_hook_run (sess, name, word, NULL, HOOK_PRINT);
}

int
plugin_emit_keypress (session *sess, unsigned int state, unsigned int keyval,
							 int len, char *string)
{
	char *word[PDIWORDS];
	char keyval_str[16];
	char state_str[16];
	char len_str[16];
	int i;

	if (!hook_list)
		return 0;

	sprintf (keyval_str, "%u", keyval);
	sprintf (state_str, "%u", state);
	sprintf (len_str, "%d", len);

	word[0] = "Key Press";
	word[1] = keyval_str;
	word[2] = state_str;
	word[3] = string;
	word[4] = len_str;
	for (i = 5; i < PDIWORDS; i++)
		word[i] = "\000";

	return plugin_hook_run (sess, word[0], word, NULL, HOOK_PRINT);
}

static int
plugin_timeout_cb (xchat_hook *hook)
{
	int ret;

	/* timer_cb's context starts as front-most-tab */
	hook->pl->context = current_sess;

	/* call the plugin's timeout function */
	ret = ((xchat_timer_cb *)hook->callback) (hook->userdata);

	/* the callback might have already unhooked it! */
	if (!g_slist_find (hook_list, hook) || hook->type == HOOK_DELETED)
		return 0;

	if (ret == 0)
	{
		hook->tag = 0;	/* avoid fe_timeout_remove, returning 0 is enough! */
		xchat_unhook (hook->pl, hook);
	}

	return ret;
}

/* insert a hook into hook_list according to its priority */

static void
plugin_insert_hook (xchat_hook *new_hook)
{
	GSList *list;
	xchat_hook *hook;

	list = hook_list;
	while (list)
	{
		hook = list->data;
		if (hook->type == new_hook->type && hook->pri <= new_hook->pri)
		{
			hook_list = g_slist_insert_before (hook_list, list, new_hook);
			return;
		}
		list = list->next;
	}

	hook_list = g_slist_append (hook_list, new_hook);
}

static gboolean
plugin_fd_cb (GIOChannel *source, GIOCondition condition, xchat_hook *hook)
{
	int flags = 0, ret;
	typedef int (xchat_fd_cb2) (int fd, int flags, void *user_data, GIOChannel *);

	if (condition & G_IO_IN)
		flags |= XCHAT_FD_READ;
	if (condition & G_IO_OUT)
		flags |= XCHAT_FD_WRITE;
	if (condition & G_IO_PRI)
		flags |= XCHAT_FD_EXCEPTION;

	ret = ((xchat_fd_cb2 *)hook->callback) (hook->pri, flags, hook->userdata, source);

	/* the callback might have already unhooked it! */
	if (!g_slist_find (hook_list, hook) || hook->type == HOOK_DELETED)
		return 0;

	if (ret == 0)
	{
		hook->tag = 0; /* avoid fe_input_remove, returning 0 is enough! */
		xchat_unhook (hook->pl, hook);
	}

	return ret;
}

/* allocate and add a hook to our list. Used for all 4 types */

static xchat_hook *
plugin_add_hook (xchat_plugin *pl, int type, int pri, const char *name,
					  const  char *help_text, void *callb, int timeout, void *userdata)
{
	xchat_hook *hook;

	hook = malloc (sizeof (xchat_hook));
	memset (hook, 0, sizeof (xchat_hook));

	hook->type = type;
	hook->pri = pri;
	if (name)
		hook->name = strdup (name);
	if (help_text)
		hook->help_text = strdup (help_text);
	hook->callback = callb;
	hook->pl = pl;
	hook->userdata = userdata;

	/* insert it into the linked list */
	plugin_insert_hook (hook);

	if (type == HOOK_TIMER)
		hook->tag = fe_timeout_add (timeout, plugin_timeout_cb, hook);

	return hook;
}

GList *
plugin_command_list(GList *tmp_list)
{
	xchat_hook *hook;
	GSList *list = hook_list;

	while (list)
	{
		hook = list->data;
		if (hook->type == HOOK_COMMAND)
			tmp_list = g_list_prepend(tmp_list, hook->name);
		list = list->next;
	}
	return tmp_list;
}

void
plugin_command_foreach (session *sess, void *userdata,
			void (*cb) (session *sess, void *userdata, char *name, char *help))
{
	GSList *list;
	xchat_hook *hook;

	list = hook_list;
	while (list)
	{
		hook = list->data;
		if (hook->type == HOOK_COMMAND && hook->name[0])
		{
			cb (sess, userdata, hook->name, hook->help_text);
		}
		list = list->next;
	}
}

int
plugin_show_help (session *sess, char *cmd)
{
	GSList *list;
	xchat_hook *hook;

	list = plugin_hook_find (hook_list, HOOK_COMMAND, cmd);
	if (list)
	{
		hook = list->data;
		if (hook->help_text)
		{
			PrintText (sess, hook->help_text);
			return 1;
		}
	}

	return 0;
}

/* ========================================================= */
/* ===== these are the functions plugins actually call ===== */
/* ========================================================= */

void *
xchat_unhook (xchat_plugin *ph, xchat_hook *hook)
{
	/* perl.c trips this */
	if (!g_slist_find (hook_list, hook) || hook->type == HOOK_DELETED)
		return NULL;

	if (hook->type == HOOK_TIMER && hook->tag != 0)
		fe_timeout_remove (hook->tag);

	if (hook->type == HOOK_FD && hook->tag != 0)
		fe_input_remove (hook->tag);

	hook->type = HOOK_DELETED;	/* expunge later */

	if (hook->name)
		free (hook->name);	/* NULL for timers & fds */
	if (hook->help_text)
		free (hook->help_text);	/* NULL for non-commands */

	return hook->userdata;
}

xchat_hook *
xchat_hook_command (xchat_plugin *ph, const char *name, int pri,
						  xchat_cmd_cb *callb, const char *help_text, void *userdata)
{
	return plugin_add_hook (ph, HOOK_COMMAND, pri, name, help_text, callb, 0,
									userdata);
}

xchat_hook *
xchat_hook_server (xchat_plugin *ph, const char *name, int pri,
						 xchat_serv_cb *callb, void *userdata)
{
	return plugin_add_hook (ph, HOOK_SERVER, pri, name, 0, callb, 0, userdata);
}

xchat_hook *
xchat_hook_print (xchat_plugin *ph, const char *name, int pri,
						xchat_print_cb *callb, void *userdata)
{
	return plugin_add_hook (ph, HOOK_PRINT, pri, name, 0, callb, 0, userdata);
}

xchat_hook *
xchat_hook_timer (xchat_plugin *ph, int timeout, xchat_timer_cb *callb,
					   void *userdata)
{
	return plugin_add_hook (ph, HOOK_TIMER, 0, 0, 0, callb, timeout, userdata);
}

xchat_hook *
xchat_hook_fd (xchat_plugin *ph, int fd, int flags,
					xchat_fd_cb *callb, void *userdata)
{
	xchat_hook *hook;

	hook = plugin_add_hook (ph, HOOK_FD, 0, 0, 0, callb, 0, userdata);
	hook->pri = fd;
	/* plugin hook_fd flags correspond exactly to FIA_* flags (fe.h) */
	hook->tag = fe_input_add (fd, flags, plugin_fd_cb, hook);

	return hook;
}

void
xchat_print (xchat_plugin *ph, const char *text)
{
	if (!is_session (ph->context))
	{
		DEBUG(PrintTextf(0, "%s\txchat_print called without a valid context.\n", ph->name));
		return;
	}

	PrintText (ph->context, (char *)text);
}

void
xchat_printf (xchat_plugin *ph, const char *format, ...)
{
	va_list args;
	char *buf;

	va_start (args, format);
	buf = g_strdup_vprintf (format, args);
	va_end (args);

	xchat_print (ph, buf);
	g_free (buf);
}

void
xchat_command (xchat_plugin *ph, const char *command)
{
	char *conv;
	int len = -1;

	if (!is_session (ph->context))
	{
		DEBUG(PrintTextf(0, "%s\txchat_command called without a valid context.\n", ph->name));
		return;
	}

	/* scripts/plugins continue to send non-UTF8... *sigh* */
	conv = text_validate ((char **)&command, &len);
	handle_command (ph->context, (char *)command, FALSE);
	g_free (conv);
}

void
xchat_commandf (xchat_plugin *ph, const char *format, ...)
{
	va_list args;
	char *buf;

	va_start (args, format);
	buf = g_strdup_vprintf (format, args);
	va_end (args);

	xchat_command (ph, buf);
	g_free (buf);
}

int
xchat_nickcmp (xchat_plugin *ph, const char *s1, const char *s2)
{
	return ((session *)ph->context)->server->p_cmp (s1, s2);
}

xchat_context *
xchat_get_context (xchat_plugin *ph)
{
	return ph->context;
}

int
xchat_set_context (xchat_plugin *ph, xchat_context *context)
{
	if (is_session (context))
	{
		ph->context = context;
		return 1;
	}
	return 0;
}

xchat_context *
xchat_find_context (xchat_plugin *ph, const char *servname, const char *channel)
{
	GSList *slist, *clist, *sessions = NULL;
	server *serv;
	session *sess;
	char *netname;

	if (servname == NULL && channel == NULL)
		return current_sess;

	slist = serv_list;
	while (slist)
	{
		serv = slist->data;
		netname = server_get_network (serv, TRUE);

		if (servname == NULL ||
			 rfc_casecmp (servname, serv->servername) == 0 ||
			 g_ascii_strcasecmp (servname, serv->hostname) == 0 ||
			 g_ascii_strcasecmp (servname, netname) == 0)
		{
			if (channel == NULL)
				return serv->front_session;

			clist = sess_list;
			while (clist)
			{
				sess = clist->data;
				if (sess->server == serv)
				{
					if (rfc_casecmp (channel, sess->channel) == 0)
					{
						if (sess->server == ph->context->server)
						{
							g_slist_free (sessions);
							return sess;
						} else
						{
							sessions = g_slist_prepend (sessions, sess);
						}
					}
				}
				clist = clist->next;
			}
		}
		slist = slist->next;
	}

	if (sessions)
	{
		sessions = g_slist_reverse (sessions);
		sess = sessions->data;
		g_slist_free (sessions);
		return sess;
	}

	return NULL;
}

const char *
xchat_get_info (xchat_plugin *ph, const char *id)
{
	session *sess;
	guint32 hash;

	/*                 1234567890 */
	if (!strncmp (id, "event_text", 10))
	{
		char *e = (char *)id + 10;
		if (*e == ' ') e++;	/* 2.8.0 only worked without a space */
		return text_find_format_string (e);
	}

	hash = str_hash (id);
	/* do the session independant ones first */
	switch (hash)
	{
	case 0x325acab5:	/* libdirfs */
		return HEXCHATLIBDIR;

	case 0x14f51cd8: /* version */
		return PACKAGE_VERSION;

	case 0xdd9b1abd:	/* xchatdir */
		return get_xdir_utf8 ();

	case 0xe33f6c4a:	/* xchatdirfs */
		return get_xdir_fs ();
	}

	sess = ph->context;
	if (!is_session (sess))
	{
		DEBUG(PrintTextf(0, "%s\txchat_get_info called without a valid context.\n", ph->name));
		return NULL;
	}

	switch (hash)
	{
	case 0x2de2ee: /* away */
		if (sess->server->is_away)
			return sess->server->last_away_reason;
		return NULL;

  	case 0x2c0b7d03: /* channel */
		return sess->channel;

	case 0x2c0d614c: /* charset */
		{
			const char *locale;

			if (sess->server->encoding)
				return sess->server->encoding;

			locale = NULL;
			g_get_charset (&locale);
			return locale;
		}

	case 0x30f5a8: /* host */
		return sess->server->hostname;

	case 0x1c0e99c1: /* inputbox */
		return fe_get_inputbox_contents (sess);

	case 0x633fb30:	/* modes */
		return sess->current_modes;

	case 0x6de15a2e:	/* network */
		return server_get_network (sess->server, FALSE);

	case 0x339763: /* nick */
		return sess->server->nick;

	case 0x438fdf9: /* nickserv */
		if (sess->server->network)
			return ((ircnet *)sess->server->network)->nickserv;
		return NULL;

	case 0xca022f43: /* server */
		if (!sess->server->connected)
			return NULL;
		return sess->server->servername;

	case 0x696cd2f: /* topic */
		return sess->topic;

	case 0x3419f12d: /* gtkwin_ptr */
		return fe_gui_info_ptr (sess, 1);

	case 0x506d600b: /* native win_ptr */
		return fe_gui_info_ptr (sess, 0);

	case 0x6d3431b5: /* win_status */
		switch (fe_gui_info (sess, 0))	/* check window status */
		{
		case 0: return "normal";
		case 1: return "active";
		case 2: return "hidden";
		}
		return NULL;
	}

	return NULL;
}

int
xchat_get_prefs (xchat_plugin *ph, const char *name, const char **string, int *integer)
{
	int i = 0;

	/* some special run-time info (not really prefs, but may aswell throw it in here) */
	switch (str_hash (name))
	{
		case 0xf82136c4: /* state_cursor */
			*integer = fe_get_inputbox_cursor (ph->context);
			return 2;

		case 0xd1b: /* id */
			*integer = ph->context->server->id;
			return 2;
	}
	
	do
	{
		if (!g_ascii_strcasecmp (name, vars[i].name))
		{
			switch (vars[i].type)
			{
			case TYPE_STR:
				*string = ((char *) &prefs + vars[i].offset);
				return 1;

			case TYPE_INT:
				*integer = *((int *) &prefs + vars[i].offset);
				return 2;

			default:
			/*case TYPE_BOOL:*/
				if (*((int *) &prefs + vars[i].offset))
					*integer = 1;
				else
					*integer = 0;
				return 3;
			}
		}
		i++;
	}
	while (vars[i].name);

	return 0;
}

xchat_list *
xchat_list_get (xchat_plugin *ph, const char *name)
{
	xchat_list *list;

	list = malloc (sizeof (xchat_list));
	list->pos = NULL;

	switch (str_hash (name))
	{
	case 0x556423d0: /* channels */
		list->type = LIST_CHANNELS;
		list->next = sess_list;
		break;

	case 0x183c4:	/* dcc */
		list->type = LIST_DCC;
		list->next = dcc_list;
		break;

	case 0xb90bfdd2:	/* ignore */
		list->type = LIST_IGNORE;
		list->next = ignore_list;
		break;

	case 0xc2079749:	/* notify */
		list->type = LIST_NOTIFY;
		list->next = notify_list;
		list->head = (void *)ph->context;	/* reuse this pointer */
		break;

	case 0x6a68e08: /* users */
		if (is_session (ph->context))
		{
			list->type = LIST_USERS;
			list->head = list->next = userlist_flat_list (ph->context);
			fe_userlist_set_selected (ph->context);
			break;
		}	/* fall through */

	default:
		free (list);
		return NULL;
	}

	return list;
}

void
xchat_list_free (xchat_plugin *ph, xchat_list *xlist)
{
	if (xlist->type == LIST_USERS)
		g_slist_free (xlist->head);
	free (xlist);
}

int
xchat_list_next (xchat_plugin *ph, xchat_list *xlist)
{
	if (xlist->next == NULL)
		return 0;

	xlist->pos = xlist->next;
	xlist->next = xlist->pos->next;

	/* NOTIFY LIST: Find the entry which matches the context
		of the plugin when list_get was originally called. */
	if (xlist->type == LIST_NOTIFY)
	{
		xlist->notifyps = notify_find_server_entry (xlist->pos->data,
													((session *)xlist->head)->server);
		if (!xlist->notifyps)
			return 0;
	}

	return 1;
}

const char * const *
xchat_list_fields (xchat_plugin *ph, const char *name)
{
	static const char * const dcc_fields[] =
	{
		"iaddress32","icps",		"sdestfile","sfile",		"snick",	"iport",
		"ipos", "iposhigh", "iresume", "iresumehigh", "isize", "isizehigh", "istatus", "itype", NULL
	};
	static const char * const channels_fields[] =
	{
		"schannel",	"schantypes", "pcontext", "iflags", "iid", "ilag", "imaxmodes",
		"snetwork", "snickmodes", "snickprefixes", "iqueue", "sserver", "itype", "iusers",
		NULL
	};
	static const char * const ignore_fields[] =
	{
		"iflags", "smask", NULL
	};
	static const char * const notify_fields[] =
	{
		"iflags", "snetworks", "snick", "toff", "ton", "tseen", NULL
	};
	static const char * const users_fields[] =
	{
		"iaway", "shost", "tlasttalk", "snick", "sprefix", "srealname", "iselected", NULL
	};
	static const char * const list_of_lists[] =
	{
		"channels",	"dcc", "ignore", "notify", "users", NULL
	};

	switch (str_hash (name))
	{
	case 0x556423d0:	/* channels */
		return channels_fields;
	case 0x183c4:		/* dcc */
		return dcc_fields;
	case 0xb90bfdd2:	/* ignore */
		return ignore_fields;
	case 0xc2079749:	/* notify */
		return notify_fields;
	case 0x6a68e08:	/* users */
		return users_fields;
	case 0x6236395:	/* lists */
		return list_of_lists;
	}

	return NULL;
}

time_t
xchat_list_time (xchat_plugin *ph, xchat_list *xlist, const char *name)
{
	guint32 hash = str_hash (name);
	gpointer data;

	switch (xlist->type)
	{
	case LIST_NOTIFY:
		if (!xlist->notifyps)
			return (time_t) -1;
		switch (hash)
		{
		case 0x1ad6f:	/* off */
			return xlist->notifyps->lastoff;
		case 0xddf:	/* on */
			return xlist->notifyps->laston;
		case 0x35ce7b:	/* seen */
			return xlist->notifyps->lastseen;
		}
		break;

	case LIST_USERS:
		data = xlist->pos->data;
		switch (hash)
		{
		case 0xa9118c42:	/* lasttalk */
			return ((struct User *)data)->lasttalk;
		}
	}

	return (time_t) -1;
}

const char *
xchat_list_str (xchat_plugin *ph, xchat_list *xlist, const char *name)
{
	guint32 hash = str_hash (name);
	gpointer data = ph->context;
	int type = LIST_CHANNELS;

	/* a NULL xlist is a shortcut to current "channels" context */
	if (xlist)
	{
		data = xlist->pos->data;
		type = xlist->type;
	}

	switch (type)
	{
	case LIST_CHANNELS:
		switch (hash)
		{
		case 0x2c0b7d03: /* channel */
			return ((session *)data)->channel;
		case 0x577e0867: /* chantypes */
			return ((session *)data)->server->chantypes;
		case 0x38b735af: /* context */
			return data;	/* this is a session * */
		case 0x6de15a2e: /* network */
			return server_get_network (((session *)data)->server, FALSE);
		case 0x8455e723: /* nickprefixes */
			return ((session *)data)->server->nick_prefixes;
		case 0x829689ad: /* nickmodes */
			return ((session *)data)->server->nick_modes;
		case 0xca022f43: /* server */
			return ((session *)data)->server->servername;
		}
		break;

	case LIST_DCC:
		switch (hash)
		{
		case 0x3d9ad31e:	/* destfile */
			return ((struct DCC *)data)->destfile;
		case 0x2ff57c:	/* file */
			return ((struct DCC *)data)->file;
		case 0x339763: /* nick */
			return ((struct DCC *)data)->nick;
		}
		break;

	case LIST_IGNORE:
		switch (hash)
		{
		case 0x3306ec:	/* mask */
			return ((struct ignore *)data)->mask;
		}
		break;

	case LIST_NOTIFY:
		switch (hash)
		{
		case 0x4e49ec05:	/* networks */
			return ((struct notify *)data)->networks;
		case 0x339763: /* nick */
			return ((struct notify *)data)->name;
		}
		break;

	case LIST_USERS:
		switch (hash)
		{
		case 0x339763: /* nick */
			return ((struct User *)data)->nick;
		case 0x30f5a8: /* host */
			return ((struct User *)data)->hostname;
		case 0xc594b292: /* prefix */
			return ((struct User *)data)->prefix;
		case 0xccc6d529: /* realname */
			return ((struct User *)data)->realname;
		}
		break;
	}

	return NULL;
}

int
xchat_list_int (xchat_plugin *ph, xchat_list *xlist, const char *name)
{
	guint32 hash = str_hash (name);
	gpointer data = ph->context;
	int tmp, type = LIST_CHANNELS;

	/* a NULL xlist is a shortcut to current "channels" context */
	if (xlist)
	{
		data = xlist->pos->data;
		type = xlist->type;
	}

	switch (type)
	{
	case LIST_DCC:
		switch (hash)
		{
		case 0x34207553: /* address32 */
			return ((struct DCC *)data)->addr;
		case 0x181a6: /* cps */
			return ((struct DCC *)data)->cps;
		case 0x349881: /* port */
			return ((struct DCC *)data)->port;
		case 0x1b254: /* pos */
			return ((struct DCC *)data)->pos & 0xffffffff;
		case 0xe8a945f6: /* poshigh */
			return (((struct DCC *)data)->pos >> 32) & 0xffffffff;
		case 0xc84dc82d: /* resume */
			return ((struct DCC *)data)->resumable & 0xffffffff;
		case 0xded4c74f: /* resumehigh */
			return (((struct DCC *)data)->resumable >> 32) & 0xffffffff;
		case 0x35e001: /* size */
			return ((struct DCC *)data)->size & 0xffffffff;
		case 0x3284d523: /* sizehigh */
			return (((struct DCC *)data)->size >> 32) & 0xffffffff;
		case 0xcacdcff2: /* status */
			return ((struct DCC *)data)->dccstat;
		case 0x368f3a: /* type */
			return ((struct DCC *)data)->type;
		}
		break;

	case LIST_IGNORE:
		switch (hash)
		{
		case 0x5cfee87:	/* flags */
			return ((struct ignore *)data)->type;
		}
		break;

	case LIST_CHANNELS:
		switch (hash)
		{
		case 0xd1b:	/* id */
			return ((struct session *)data)->server->id;
		case 0x5cfee87:	/* flags */
			tmp = ((struct session *)data)->alert_taskbar;   /* bit 10 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->alert_tray;         /* 9 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->alert_beep;         /* 8 */
			tmp <<= 1;
			/*tmp |= ((struct session *)data)->color_paste;*/    /* 7 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->text_hidejoinpart;   /* 6 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->have_idmsg; /* 5 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->have_whox;  /* 4 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->end_of_motd;/* 3 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->is_away;    /* 2 */
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->connecting; /* 1 */ 
			tmp <<= 1;
			tmp |= ((struct session *)data)->server->connected;  /* 0 */
			return tmp;
		case 0x1a192: /* lag */
			return ((struct session *)data)->server->lag;
		case 0x1916144c: /* maxmodes */
			return ((struct session *)data)->server->modes_per_line;
		case 0x66f1911: /* queue */
			return ((struct session *)data)->server->sendq_len;
		case 0x368f3a:	/* type */
			return ((struct session *)data)->type;
		case 0x6a68e08: /* users */
			return ((struct session *)data)->total;
		}
		break;

	case LIST_NOTIFY:
		if (!xlist->notifyps)
			return -1;
		switch (hash)
		{
		case 0x5cfee87: /* flags */
			return xlist->notifyps->ison;
		}

	case LIST_USERS:
		switch (hash)
		{
		case 0x2de2ee:	/* away */
			return ((struct User *)data)->away;
		case 0x4705f29b: /* selected */
			return ((struct User *)data)->selected;
		}
		break;

	}

	return -1;
}

void *
xchat_plugingui_add (xchat_plugin *ph, const char *filename,
							const char *name, const char *desc,
							const char *version, char *reserved)
{
#ifdef USE_PLUGIN
	ph = plugin_list_add (NULL, strdup (filename), strdup (name), strdup (desc),
								 strdup (version), NULL, NULL, TRUE, TRUE);
	fe_pluginlist_update ();
#endif

	return ph;
}

void
xchat_plugingui_remove (xchat_plugin *ph, void *handle)
{
#ifdef USE_PLUGIN
	plugin_free (handle, FALSE, FALSE);
#endif
}

int
xchat_emit_print (xchat_plugin *ph, const char *event_name, ...)
{
	va_list args;
	/* currently only 4 because no events use more than 4.
		This can be easily expanded without breaking the API. */
	char *argv[4] = {NULL, NULL, NULL, NULL};
	int i = 0;

	va_start (args, event_name);
	while (1)
	{
		argv[i] = va_arg (args, char *);
		if (!argv[i])
			break;
		i++;
		if (i >= 4)
			break;
	}

	i = text_emit_by_name ((char *)event_name, ph->context, argv[0], argv[1],
								  argv[2], argv[3]);
	va_end (args);

	return i;
}

char *
xchat_gettext (xchat_plugin *ph, const char *msgid)
{
	/* so that plugins can use xchat's internal gettext strings. */
	/* e.g. The EXEC plugin uses this on Windows. */
	return _(msgid);
}

void
xchat_send_modes (xchat_plugin *ph, const char **targets, int ntargets, int modes_per_line, char sign, char mode)
{
	char tbuf[514];	/* modes.c needs 512 + null */

	send_channel_modes (ph->context, tbuf, (char **)targets, 0, ntargets, sign, mode, modes_per_line);
}

char *
xchat_strip (xchat_plugin *ph, const char *str, int len, int flags)
{
	return strip_color ((char *)str, len, flags);
}

void
xchat_free (xchat_plugin *ph, void *ptr)
{
	g_free (ptr);
}

static int
xchat_pluginpref_set_str_real (xchat_plugin *pl, const char *var, const char *value, int mode) /* mode: 0 = delete, 1 = save */
{
	FILE *fpIn;
	int fhOut;
	int prevSetting;
	char confname[64];
	char confname_tmp[69];
	char buffer[512];		/* the same as in cfg_put_str */
	char buffer_tmp[512];
	char *canon;

	canon = g_strdup (pl->name);
	canonalize_key (canon);
	sprintf (confname, "addon_%s.conf", canon);
	g_free (canon);
	sprintf (confname_tmp, "%s.new", confname);

	fhOut = xchat_open_file (confname_tmp, O_TRUNC | O_WRONLY | O_CREAT, 0600, XOF_DOMODE);
	fpIn = xchat_fopen_file (confname, "r", 0);

	if (fhOut == -1)		/* unable to save, abort */
	{
		return 0;
	}
	else if (fpIn == NULL)	/* no previous config file, no parsing */
	{
		if (mode)
		{
			sprintf (buffer, "%s = %s\n", var, value);
			write (fhOut, buffer, strlen (buffer));
			close (fhOut);

			sprintf (buffer, "%s/%s", get_xdir_fs (), confname);
			sprintf (buffer_tmp, "%s/%s", get_xdir_fs (), confname_tmp);

#ifdef WIN32
			unlink (buffer);
#endif

			if (rename (buffer_tmp, buffer) == 0)
			{
				return 1;
			}
			else
			{
				return 0;
			}
		}
		else
		{
			/* mode = 0, we want to delete but the config file and thus the given setting does not exist, we're ready */
			close (fhOut);
			return 1;
		}
	}
	else	/* existing config file, preserve settings and find & replace current var value if any */
	{
		prevSetting = 0;

		while (fscanf (fpIn, " %[^\n]", &buffer) != EOF)	/* read whole lines including whitespaces */
		{
			sprintf (buffer_tmp, "%s ", var);				/* add one space, this way it works against var - var2 checks too */

			if (strncmp (buffer_tmp, buffer, strlen (var) + 1) == 0)	/* given setting already exists */
			{
				if (mode)									/* overwrite the existing matching setting if we are in save mode */
				{
					sprintf (buffer, "%s = %s\n", var, value);
				}
				else										/* erase the setting in delete mode */
				{
					strcpy (buffer, "");
				}

				prevSetting = 1;
			}
			else
			{
				strcat (buffer, "\n");						/* preserve the existing different settings */
			}

			write (fhOut, buffer, strlen (buffer));
		}

		fclose (fpIn);

		if (!prevSetting && mode)	/* var doesn't exist currently, append if we're in save mode */
		{
			sprintf (buffer, "%s = %s\n", var, value);
			write (fhOut, buffer, strlen (buffer));
		}

		close (fhOut);

		sprintf (buffer, "%s/%s", get_xdir_fs (), confname);
		sprintf (buffer_tmp, "%s/%s", get_xdir_fs (), confname_tmp);

#ifdef WIN32
		unlink (buffer);
#endif

		if (rename (buffer_tmp, buffer) == 0)
		{
			return 1;
		}
		else
		{
			return 0;
		}
	}
}

int
xchat_pluginpref_set_str (xchat_plugin *pl, const char *var, const char *value)
{
	return xchat_pluginpref_set_str_real (pl, var, value, 1);
}

int
xchat_pluginpref_get_str (xchat_plugin *pl, const char *var, char *dest)
{
	int fh;
	int l;
	char confname[64];
	char *canon;
	char *cfg;
	struct stat st;

	canon = g_strdup (pl->name);
	canonalize_key (canon);
	sprintf (confname, "addon_%s.conf", canon);
	g_free (canon);

	/* partly borrowed from palette.c */
	fh = xchat_open_file (confname, O_RDONLY, 0, 0);

	if (fh == -1)
	{
		return 0;
	}

	fstat (fh, &st);
	cfg = malloc (st.st_size + 1);

	if (!cfg)
	{
		close (fh);
		return 0;
	}

	cfg[0] = '\0';
	l = read (fh, cfg, st.st_size);

	if (l >= 0)
	{
		cfg[l] = '\0';
	}

	if (!cfg_get_str (cfg, var, dest, 512)) /* dest_len is the same as buffer size in set */
	{
		free (cfg);
		close (fh);
		return 0;
	}

	free (cfg);
	close (fh);
	return 1;
}

int
xchat_pluginpref_set_int (xchat_plugin *pl, const char *var, int value)
{
	char buffer[12];

	sprintf (buffer, "%d", value);
	return xchat_pluginpref_set_str_real (pl, var, buffer, 1);
}

int
xchat_pluginpref_get_int (xchat_plugin *pl, const char *var)
{
	char buffer[12];

	if (xchat_pluginpref_get_str (pl, var, buffer))
	{
		return atoi (buffer);
	}
	else
	{
		return -1;
	}
}

int
xchat_pluginpref_delete (xchat_plugin *pl, const char *var)
{
	return xchat_pluginpref_set_str_real (pl, var, 0, 0);
}

int
xchat_pluginpref_list (xchat_plugin *pl, char* dest)
{
	FILE *fpIn;
	char confname[64];
	char buffer[512];										/* the same as in cfg_put_str */
	char *token;

	token = g_strdup (pl->name);
	canonalize_key (token);
	sprintf (confname, "addon_%s.conf", token);
	g_free (token);

	fpIn = xchat_fopen_file (confname, "r", 0);

	if (fpIn == NULL)										/* no existing config file, no parsing */
	{
		return 0;
	}
	else													/* existing config file, get list of settings */
	{
		strcpy (dest, "");									/* clean up garbage */
		while (fscanf (fpIn, " %[^\n]", &buffer) != EOF)	/* read whole lines including whitespaces */
		{
			token = strtok (buffer, "=");
			strncat (dest, token, strlen (token) - 1);
			strcat (dest, ",");
		}

		fclose (fpIn);
	}

	return 1;
}
