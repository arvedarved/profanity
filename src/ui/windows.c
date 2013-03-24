/*
 * windows.c
 *
 * Copyright (C) 2012, 2013 James Booth <boothj5@gmail.com>
 *
 * This file is part of Profanity.
 *
 * Profanity is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Profanity is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Profanity.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_LIBXSS
#include <X11/extensions/scrnsaver.h>
#endif
#include <glib.h>
#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif
#ifdef PLATFORM_CYGWIN
#include <windows.h>
#endif
#ifdef HAVE_NCURSESW_NCURSES_H
#include <ncursesw/ncurses.h>
#elif HAVE_NCURSES_H
#include <ncurses.h>
#endif

#include "chat_session.h"
#include "command/command.h"
#include "common.h"
#include "config/preferences.h"
#include "config/theme.h"
#include "contact.h"
#include "contact_list.h"
#include "jid.h"
#include "log.h"
#include "muc.h"
#include "ui/ui.h"
#include "ui/window.h"

#define CONS_WIN_TITLE "_cons"
#define NUM_WINS 10

// holds console at index 0 and chat wins 1 through to 9
static ProfWin* windows[NUM_WINS];

// the window currently being displayed
static int current_index = 0;
static ProfWin *current;
static ProfWin *console;

// current window state
static int dirty;

// max columns for main windows, never resize below
static int max_cols = 0;

static char *win_title;

#ifdef HAVE_LIBXSS
static Display *display;
#endif

static GTimer *ui_idle_time;

static void _set_current(int index);
static void _create_windows(void);
static void _cons_splash_logo(void);
static void _cons_show_basic_help(void);
static void _win_show_contact(ProfWin *window, PContact contact);
static int _find_prof_win_index(const char * const contact);
static int _new_prof_win(const char * const contact, win_type_t type);
static void _current_window_refresh(void);
static void _win_show_time(WINDOW *win, char showchar);
static void _win_show_user(WINDOW *win, const char * const user, const int colour);
static void _win_show_message(WINDOW *win, const char * const message);
static void _win_show_error_msg(WINDOW *win, const char * const message);
static void _show_status_string(WINDOW *win, const char * const from,
    const char * const show, const char * const status,
    GDateTime *last_activity, const char * const pre,
    const char * const default_show);
static void _cons_show_typing(const char * const short_from);
static void _cons_show_incoming_message(const char * const short_from,
    const int win_index);
static void _win_handle_switch(const wint_t * const ch);
static void _win_handle_page(const wint_t * const ch);
static void _win_resize_all(void);
static gint _win_get_unread(void);
static void _win_show_history(WINDOW *win, int win_index,
    const char * const contact);
static void _win_show_info(WINDOW *win, PContact pcontact);
static gboolean _new_release(char *found_version);
static void _ui_draw_win_title(void);
static void _presence_colour_on(WINDOW *win, const char * const presence);
static void _presence_colour_off(WINDOW *win, const char * const presence);

static void _notify(const char * const message, int timeout,
    const char * const category);
static void _notify_remind(gint unread);
static void _notify_message(const char * const short_from);
static void _notify_typing(const char * const from);

void
ui_init(void)
{
    log_info("Initialising UI");
    initscr();
    raw();
    keypad(stdscr, TRUE);
    if (prefs_get_boolean(PREF_MOUSE)) {
        mousemask(ALL_MOUSE_EVENTS, NULL);
        mouseinterval(5);
    }
    ui_load_colours();
    refresh();
    create_title_bar();
    create_status_bar();
    status_bar_active(0);
    create_input_window();
    _create_windows();
#ifdef HAVE_LIBXSS
    display = XOpenDisplay(0);
#endif
    ui_idle_time = g_timer_new();
    dirty = TRUE;
}

void
ui_refresh(void)
{
    _ui_draw_win_title();

    title_bar_refresh();
    status_bar_refresh();

    if (dirty) {
        _current_window_refresh();
        dirty = FALSE;
    }

    inp_put_back();
}

static void
_ui_draw_win_title(void)
{
    char new_win_title[100];

    GString *version_str = g_string_new("");

    if (prefs_get_boolean(PREF_TITLEBARVERSION)) {
        g_string_append(version_str, " ");
        g_string_append(version_str, PACKAGE_VERSION);
        if (strcmp(PACKAGE_STATUS, "development") == 0) {
            g_string_append(version_str, "dev");
        }
    }

    jabber_conn_status_t status = jabber_get_connection_status();

    if (status == JABBER_CONNECTED) {
        const char * const jid = jabber_get_jid();
        gint unread = _win_get_unread();

        if (unread != 0) {
            snprintf(new_win_title, sizeof(new_win_title), "%c]0;%s%s (%d) - %s%c", '\033', "Profanity", version_str->str, unread, jid, '\007');
        } else {
            snprintf(new_win_title, sizeof(new_win_title), "%c]0;%s%s - %s%c", '\033', "Profanity", version_str->str, jid, '\007');
        }
    } else {
        snprintf(new_win_title, sizeof(new_win_title), "%c]0;%s%s%c", '\033', "Profanity", version_str->str, '\007');
    }

    g_string_free(version_str, TRUE);

    if (g_strcmp0(win_title, new_win_title) != 0) {
        // print to x-window title bar
        printf("%s", new_win_title);
        if (win_title != NULL) {
            free(win_title);
        }
        win_title = strdup(new_win_title);
    }
}

unsigned long
ui_get_idle_time(void)
{
// if compiled with libxss, get the x sessions idle time
#ifdef HAVE_LIBXSS
    XScreenSaverInfo *info = XScreenSaverAllocInfo();
    if (info != NULL && display != NULL) {
        XScreenSaverQueryInfo(display, DefaultRootWindow(display), info);
        unsigned long result = info->idle;
        XFree(info);
        return result;
    }
// if no libxss or xss idle time failed, use profanity idle time
#endif
    gdouble seconds_elapsed = g_timer_elapsed(ui_idle_time, NULL);
    unsigned long ms_elapsed = seconds_elapsed * 1000.0;
    return ms_elapsed;
}

void
ui_reset_idle_time(void)
{
    g_timer_start(ui_idle_time);
}

void
ui_close(void)
{
#ifdef HAVE_LIBNOTIFY
    if (notify_is_initted()) {
        notify_uninit();
    }
#endif
    endwin();
}

void
ui_resize(const int ch, const char * const input, const int size)
{
    log_info("Resizing UI");
    title_bar_resize();
    status_bar_resize();
    _win_resize_all();
    inp_win_resize(input, size);
    dirty = TRUE;
}

void
ui_load_colours(void)
{
    if (has_colors()) {
        use_default_colors();
        start_color();
        theme_init_colours();
    }
}

gboolean
ui_windows_full(void)
{
    int i;
    for (i = 1; i < NUM_WINS; i++) {
        if (windows[i] == NULL) {
            return FALSE;
        }
    }

    return TRUE;
}

void
ui_show_typing(const char * const from)
{
    int win_index = _find_prof_win_index(from);

    if (prefs_get_boolean(PREF_INTYPE)) {
        // no chat window for user
        if (win_index == NUM_WINS) {
            _cons_show_typing(from);

        // have chat window but not currently in it
        } else if (win_index != current_index) {
            _cons_show_typing(from);
            dirty = TRUE;

        // in chat window with user
        } else {
            title_bar_set_typing(TRUE);
            title_bar_draw();

            status_bar_active(win_index);
            dirty = TRUE;
       }
    }

    if (prefs_get_boolean(PREF_NOTIFY_TYPING))
        _notify_typing(from);
}

void
ui_idle(void)
{
    int i;

    // loop through regular chat windows and update states
    for (i = 1; i < NUM_WINS; i++) {
        if ((windows[i] != NULL) && (windows[i]->type == WIN_CHAT)) {
            char *recipient = windows[i]->from;
            chat_session_no_activity(recipient);

            if (chat_session_is_gone(recipient) &&
                    !chat_session_get_sent(recipient)) {
                message_send_gone(recipient);
            } else if (chat_session_is_inactive(recipient) &&
                    !chat_session_get_sent(recipient)) {
                message_send_inactive(recipient);
            } else if (prefs_get_boolean(PREF_OUTTYPE) &&
                    chat_session_is_paused(recipient) &&
                    !chat_session_get_sent(recipient)) {
                message_send_paused(recipient);
            }
        }
    }
}

void
ui_show_incoming_msg(const char * const from, const char * const message,
    GTimeVal *tv_stamp, gboolean priv)
{
    char *display_from;
    win_type_t win_type;
    if (priv) {
        win_type = WIN_PRIVATE;
        display_from = get_nick_from_full_jid(from);
    } else {
        win_type = WIN_CHAT;
        display_from = strdup(from);
    }

    int win_index = _find_prof_win_index(from);
    if (win_index == NUM_WINS)
        win_index = _new_prof_win(from, win_type);

    // no spare windows left
    if (win_index == 0) {
        if (tv_stamp == NULL) {
            _win_show_time(console->win, '-');
        } else {
            GDateTime *time = g_date_time_new_from_timeval_utc(tv_stamp);
            gchar *date_fmt = g_date_time_format(time, "%H:%M:%S");
            wattron(console->win, COLOUR_TIME);
            wprintw(console->win, "%s - ", date_fmt);
            wattroff(console->win, COLOUR_TIME);
            g_date_time_unref(time);
            g_free(date_fmt);
        }

        if (strncmp(message, "/me ", 4) == 0) {
            wattron(console->win, COLOUR_THEM);
            wprintw(console->win, "*%s ", from);
            wprintw(console->win, "%s", message + 4);
            wprintw(console->win, "\n");
            wattroff(console->win, COLOUR_THEM);
        } else {
            _win_show_user(console->win, from, 1);
            _win_show_message(console->win, message);
        }

        cons_bad_show("Windows all used, close a window to respond.");

        if (current_index == 0) {
           dirty = TRUE;
        } else {
            status_bar_new(0);
        }

    // window found or created
    } else {
        WINDOW *win = windows[win_index]->win;

        // currently viewing chat window with sender
        if (win_index == current_index) {
            if (tv_stamp == NULL) {
                _win_show_time(win, '-');
            } else {
                GDateTime *time = g_date_time_new_from_timeval_utc(tv_stamp);
                gchar *date_fmt = g_date_time_format(time, "%H:%M:%S");
                wattron(win, COLOUR_TIME);
                wprintw(win, "%s - ", date_fmt);
                wattroff(win, COLOUR_TIME);
                g_date_time_unref(time);
                g_free(date_fmt);
            }

            if (strncmp(message, "/me ", 4) == 0) {
                wattron(win, COLOUR_THEM);
                wprintw(win, "*%s ", display_from);
                wprintw(win, "%s", message + 4);
                wprintw(win, "\n");
                wattroff(win, COLOUR_THEM);
            } else {
                _win_show_user(win, display_from, 1);
                _win_show_message(win, message);
            }
            title_bar_set_typing(FALSE);
            title_bar_draw();
            status_bar_active(win_index);
            dirty = TRUE;

        // not currently viewing chat window with sender
        } else {
            status_bar_new(win_index);
            _cons_show_incoming_message(from, win_index);
            if (prefs_get_boolean(PREF_FLASH))
                flash();

            windows[win_index]->unread++;
            if (prefs_get_boolean(PREF_CHLOG) && prefs_get_boolean(PREF_HISTORY)) {
                _win_show_history(win, win_index, from);
            }

            if (tv_stamp == NULL) {
                _win_show_time(win, '-');
            } else {
                GDateTime *time = g_date_time_new_from_timeval_utc(tv_stamp);
                gchar *date_fmt = g_date_time_format(time, "%H:%M:%S");
                wattron(win, COLOUR_TIME);
                wprintw(win, "%s - ", date_fmt);
                wattroff(win, COLOUR_TIME);
                g_date_time_unref(time);
                g_free(date_fmt);
            }

            if (strncmp(message, "/me ", 4) == 0) {
                wattron(win, COLOUR_THEM);
                wprintw(win, "*%s ", display_from);
                wprintw(win, "%s", message + 4);
                wprintw(win, "\n");
                wattroff(win, COLOUR_THEM);
            } else {
                _win_show_user(win, display_from, 1);
                _win_show_message(win, message);
            }
        }
    }

    if (prefs_get_boolean(PREF_BEEP))
        beep();
    if (prefs_get_boolean(PREF_NOTIFY_MESSAGE))
        _notify_message(display_from);

    g_free(display_from);
}

void
ui_contact_online(const char * const barejid, const char * const resource,
    const char * const show, const char * const status, GDateTime *last_activity)
{
    Jid *jid = jid_create_from_bare_and_resource(barejid, resource);
    _show_status_string(console->win, jid->fulljid, show, status, last_activity, "++",
        "online");

    int win_index = _find_prof_win_index(barejid);
    if (win_index != NUM_WINS) {
        WINDOW *win = windows[win_index]->win;
        _show_status_string(win, jid->fulljid, show, status, last_activity, "++",
            "online");
    }

    jid_destroy(jid);

    if (win_index == current_index)
        dirty = TRUE;
}

void
ui_contact_offline(const char * const from, const char * const show,
    const char * const status)
{
    _show_status_string(console->win, from, show, status, NULL, "--", "offline");

    int win_index = _find_prof_win_index(from);
    if (win_index != NUM_WINS) {
        WINDOW *win = windows[win_index]->win;
        _show_status_string(win, from, show, status, NULL, "--", "offline");
    }

    if (win_index == current_index)
        dirty = TRUE;
}

void
ui_disconnected(void)
{
    int i;
    // show message in all active chats
    for (i = 1; i < NUM_WINS; i++) {
        if (windows[i] != NULL) {
            WINDOW *win = windows[i]->win;
            _win_show_time(win, '-');
            wattron(win, COLOUR_ERROR);
            wprintw(win, "%s\n", "Lost connection.");
            wattroff(win, COLOUR_ERROR);

            // if current win, set dirty
            if (i == current_index) {
                dirty = TRUE;
            }
        }
    }

    title_bar_set_status(CONTACT_OFFLINE);
    status_bar_clear_message();
    status_bar_refresh();
}

void
ui_handle_special_keys(const wint_t * const ch, const char * const inp,
    const int size)
{
    _win_handle_switch(ch);
    _win_handle_page(ch);
    if (*ch == KEY_RESIZE) {
        ui_resize(*ch, inp, size);
    }

}

void
ui_switch_win(const int i)
{
    win_current_page_off();
    if (windows[i] != NULL) {
        current_index = i;
        current = windows[current_index];
        win_current_page_off();

        current->unread = 0;

        if (i == 0) {
            title_bar_title();
            status_bar_active(0);
        } else {
            title_bar_set_recipient(current->from);
            title_bar_draw();;
            status_bar_active(i);
        }
    }

    dirty = TRUE;
}

void
win_current_clear(void)
{
    wclear(current->win);
    dirty = TRUE;
}

void
win_current_close(void)
{
    window_free(current);
    windows[current_index] = NULL;

    // set it as inactive in the status bar
    status_bar_inactive(current_index);

    // go back to console window
    _set_current(0);
    status_bar_active(0);
    title_bar_title();

    dirty = TRUE;
}

int
win_current_is_console(void)
{
    return (current->type == WIN_CONSOLE);
}

int
win_current_is_chat(void)
{
    return (current->type == WIN_CHAT);
}

int
win_current_is_groupchat(void)
{
    return (current->type == WIN_MUC);
}

int
win_current_is_private(void)
{
    return (current->type == WIN_PRIVATE);
}

char *
win_current_get_recipient(void)
{
    return strdup(current->from);
}

void
win_current_show(const char * const msg, ...)
{
    va_list arg;
    va_start(arg, msg);
    GString *fmt_msg = g_string_new(NULL);
    g_string_vprintf(fmt_msg, msg, arg);
    _win_show_time(current->win, '-');
    wprintw(current->win, "%s\n", fmt_msg->str);
    g_string_free(fmt_msg, TRUE);
    va_end(arg);

    dirty = TRUE;
}

void
win_current_bad_show(const char * const msg)
{
    WINDOW *win = current->win;
    _win_show_time(win, '-');
    wattron(win, COLOUR_ERROR);
    wprintw(win, "%s\n", msg);
    wattroff(win, COLOUR_ERROR);

    dirty = TRUE;
}

void
win_current_page_off(void)
{
    int rows = getmaxy(stdscr);
    ProfWin *window = windows[current_index];

    window->paged = 0;

    int y = getcury(window->win);

    int size = rows - 3;

    window->y_pos = y - (size - 1);
    if (window->y_pos < 0)
        window->y_pos = 0;

    dirty = TRUE;
}

void
win_show_error_msg(const char * const from, const char *err_msg)
{
    int win_index;
    WINDOW *win;

    if (from == NULL || err_msg == NULL)
        return;

    win_index = _find_prof_win_index(from);
    // chat window exists
    if (win_index < NUM_WINS) {
        win = windows[win_index]->win;
        _win_show_time(win, '-');
        _win_show_error_msg(win, err_msg);
        if (win_index == current_index) {
            dirty = TRUE;
        }
    }
}

void
win_show_system_msg(const char * const from, const char *message)
{
    int win_index;
    WINDOW *win;
    char from_cpy[strlen(from) + 1];
    char *bare_jid;

    if (from == NULL || message == NULL)
        return;

    strcpy(from_cpy, from);
    bare_jid = strtok(from_cpy, "/");

    win_index = _find_prof_win_index(bare_jid);
    if (win_index == NUM_WINS) {
        win_index = _new_prof_win(bare_jid, WIN_CHAT);
        status_bar_active(win_index);
        dirty = TRUE;
    }
    win = windows[win_index]->win;

    _win_show_time(win, '-');
    wprintw(win, "*%s %s\n", bare_jid, message);

    // this is the current window
    if (win_index == current_index) {
        dirty = TRUE;
    }
}

void
win_show_gone(const char * const from)
{
    int win_index;
    WINDOW *win;

    if (from == NULL)
        return;

    win_index = _find_prof_win_index(from);
    // chat window exists
    if (win_index < NUM_WINS) {
        win = windows[win_index]->win;
        _win_show_time(win, '-');
        wattron(win, COLOUR_GONE);
        wprintw(win, "*%s ", from);
        wprintw(win, "has left the conversation.");
        wprintw(win, "\n");
        wattroff(win, COLOUR_GONE);
        if (win_index == current_index) {
            dirty = TRUE;
        }
    }
}

void
win_new_chat_win(const char * const to)
{
    // if the contact is offline, show a message
    PContact contact = contact_list_get_contact(to);
    int win_index = _find_prof_win_index(to);
    WINDOW *win = NULL;

    // create new window
    if (win_index == NUM_WINS) {
        Jid *jid = jid_create(to);

        if (muc_room_is_active(jid)) {
            win_index = _new_prof_win(to, WIN_PRIVATE);
        } else {
            win_index = _new_prof_win(to, WIN_CHAT);
        }

        jid_destroy(jid);

        win = windows[win_index]->win;

        if (prefs_get_boolean(PREF_CHLOG) && prefs_get_boolean(PREF_HISTORY)) {
            _win_show_history(win, win_index, to);
        }

        if (contact != NULL) {
            if (strcmp(p_contact_presence(contact), "offline") == 0) {
                const char const *show = p_contact_presence(contact);
                const char const *status = p_contact_status(contact);
                _show_status_string(win, to, show, status, NULL, "--", "offline");
            }
        }

    // use existing window
    } else {
        win = windows[win_index]->win;
    }

    ui_switch_win(win_index);
}

void
win_show_outgoing_msg(const char * const from, const char * const to,
    const char * const message)
{
    // if the contact is offline, show a message
    PContact contact = contact_list_get_contact(to);
    int win_index = _find_prof_win_index(to);
    WINDOW *win = NULL;

    // create new window
    if (win_index == NUM_WINS) {
        Jid *jid = jid_create(to);

        if (muc_room_is_active(jid)) {
            win_index = _new_prof_win(to, WIN_PRIVATE);
        } else {
            win_index = _new_prof_win(to, WIN_CHAT);
        }

        jid_destroy(jid);

        win = windows[win_index]->win;

        if (prefs_get_boolean(PREF_CHLOG) && prefs_get_boolean(PREF_HISTORY)) {
            _win_show_history(win, win_index, to);
        }

        if (contact != NULL) {
            if (strcmp(p_contact_presence(contact), "offline") == 0) {
                const char const *show = p_contact_presence(contact);
                const char const *status = p_contact_status(contact);
                _show_status_string(win, to, show, status, NULL, "--", "offline");
            }
        }

    // use existing window
    } else {
        win = windows[win_index]->win;
    }

    _win_show_time(win, '-');
    if (strncmp(message, "/me ", 4) == 0) {
        wattron(win, COLOUR_ME);
        wprintw(win, "*%s ", from);
        wprintw(win, "%s", message + 4);
        wprintw(win, "\n");
        wattroff(win, COLOUR_ME);
    } else {
        _win_show_user(win, from, 0);
        _win_show_message(win, message);
    }
    ui_switch_win(win_index);
}

void
win_join_chat(Jid *jid)
{
    int win_index = _find_prof_win_index(jid->barejid);

    // create new window
    if (win_index == NUM_WINS) {
        win_index = _new_prof_win(jid->barejid, WIN_MUC);
    }

    ui_switch_win(win_index);
}

void
win_show_room_roster(const char * const room, GList *roster, const char * const presence)
{
    int win_index = _find_prof_win_index(room);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '!');
    if ((roster == NULL) || (g_list_length(roster) == 0)) {
        wattron(win, COLOUR_ROOMINFO);
        if (presence == NULL) {
            wprintw(win, "Room is empty.\n");
        } else {
            wprintw(win, "No participants are %s.\n", presence);
        }
        wattroff(win, COLOUR_ROOMINFO);
    } else {
        wattron(win, COLOUR_ROOMINFO);
        if (presence == NULL) {
            wprintw(win, "Participants: ");
        } else {
            wprintw(win, "Participants (%s): ", presence);
        }
        wattroff(win, COLOUR_ROOMINFO);
        wattron(win, COLOUR_ONLINE);

        while (roster != NULL) {
            PContact member = roster->data;
            const char const *nick = p_contact_barejid(member);
            const char const *show = p_contact_presence(member);

            _presence_colour_on(win, show);
            wprintw(win, "%s", nick);
            _presence_colour_off(win, show);

            if (roster->next != NULL) {
                wprintw(win, ", ");
            }

            roster = g_list_next(roster);
        }

        wprintw(win, "\n");
        wattroff(win, COLOUR_ONLINE);
    }

    if (win_index == current_index)
        dirty = TRUE;
}

void
win_show_room_member_offline(const char * const room, const char * const nick)
{
    int win_index = _find_prof_win_index(room);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '!');
    wattron(win, COLOUR_OFFLINE);
    wprintw(win, "<- %s has left the room.\n", nick);
    wattroff(win, COLOUR_OFFLINE);

    if (win_index == current_index)
        dirty = TRUE;
}

void
win_show_room_member_online(const char * const room, const char * const nick,
    const char * const show, const char * const status)
{
    int win_index = _find_prof_win_index(room);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '!');
    wattron(win, COLOUR_ONLINE);
    wprintw(win, "-> %s has joined the room.\n", nick);
    wattroff(win, COLOUR_ONLINE);

    if (win_index == current_index)
        dirty = TRUE;
}

void
win_show_room_member_presence(const char * const room, const char * const nick,
    const char * const show, const char * const status)
{
    int win_index = _find_prof_win_index(room);
    if (win_index != NUM_WINS) {
        WINDOW *win = windows[win_index]->win;
        _show_status_string(win, nick, show, status, NULL, "++", "online");
    }

    if (win_index == current_index)
        dirty = TRUE;
}

void
win_show_room_member_nick_change(const char * const room,
    const char * const old_nick, const char * const nick)
{
    int win_index = _find_prof_win_index(room);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '!');
    wattron(win, COLOUR_THEM);
    wprintw(win, "** %s is now known as %s\n", old_nick, nick);
    wattroff(win, COLOUR_THEM);

    if (win_index == current_index)
        dirty = TRUE;
}

void
win_show_room_nick_change(const char * const room, const char * const nick)
{
    int win_index = _find_prof_win_index(room);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '!');
    wattron(win, COLOUR_ME);
    wprintw(win, "** You are now known as %s\n", nick);
    wattroff(win, COLOUR_ME);

    if (win_index == current_index)
        dirty = TRUE;
}

void
win_show_room_history(const char * const room_jid, const char * const nick,
    GTimeVal tv_stamp, const char * const message)
{
    int win_index = _find_prof_win_index(room_jid);
    WINDOW *win = windows[win_index]->win;

    GDateTime *time = g_date_time_new_from_timeval_utc(&tv_stamp);
    gchar *date_fmt = g_date_time_format(time, "%H:%M:%S");
    wprintw(win, "%s - ", date_fmt);
    g_date_time_unref(time);
    g_free(date_fmt);

    if (strncmp(message, "/me ", 4) == 0) {
        wprintw(win, "*%s ", nick);
        wprintw(win, "%s", message + 4);
        wprintw(win, "\n");
    } else {
        wprintw(win, "%s: ", nick);
        _win_show_message(win, message);
    }

    if (win_index == current_index)
        dirty = TRUE;
}

void
win_show_room_message(const char * const room_jid, const char * const nick,
    const char * const message)
{
    int win_index = _find_prof_win_index(room_jid);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '-');
    if (strcmp(nick, muc_get_room_nick(room_jid)) != 0) {
        if (strncmp(message, "/me ", 4) == 0) {
            wattron(win, COLOUR_THEM);
            wprintw(win, "*%s ", nick);
            wprintw(win, "%s", message + 4);
            wprintw(win, "\n");
            wattroff(win, COLOUR_THEM);
        } else {
            _win_show_user(win, nick, 1);
            _win_show_message(win, message);
        }

    } else {
        if (strncmp(message, "/me ", 4) == 0) {
            wattron(win, COLOUR_ME);
            wprintw(win, "*%s ", nick);
            wprintw(win, "%s", message + 4);
            wprintw(win, "\n");
            wattroff(win, COLOUR_ME);
        } else {
            _win_show_user(win, nick, 0);
            _win_show_message(win, message);
        }
    }

    // currently in groupchat window
    if (win_index == current_index) {
        status_bar_active(win_index);
        dirty = TRUE;

    // not currenlty on groupchat window
    } else {
        status_bar_new(win_index);
        _cons_show_incoming_message(nick, win_index);
        if (current_index == 0) {
            dirty = TRUE;
        }

        if (strcmp(nick, muc_get_room_nick(room_jid)) != 0) {
            if (prefs_get_boolean(PREF_FLASH)) {
                flash();
            }
        }

        windows[win_index]->unread++;
    }

    if (strcmp(nick, muc_get_room_nick(room_jid)) != 0) {
        if (prefs_get_boolean(PREF_BEEP)) {
            beep();
        }
        if (prefs_get_boolean(PREF_NOTIFY_MESSAGE)) {
            _notify_message(nick);
        }
    }
}

void
win_show_room_subject(const char * const room_jid, const char * const subject)
{
    int win_index = _find_prof_win_index(room_jid);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '!');
    wattron(win, COLOUR_ROOMINFO);
    wprintw(win, "Room subject: ");
    wattroff(win, COLOUR_ROOMINFO);
    wprintw(win, "%s\n", subject);

    // currently in groupchat window
    if (win_index == current_index) {
        status_bar_active(win_index);
        dirty = TRUE;

    // not currenlty on groupchat window
    } else {
        status_bar_new(win_index);
    }
}

void
win_show_room_broadcast(const char * const room_jid, const char * const message)
{
    int win_index = _find_prof_win_index(room_jid);
    WINDOW *win = windows[win_index]->win;

    _win_show_time(win, '!');
    wattron(win, COLOUR_ROOMINFO);
    wprintw(win, "Room message: ");
    wattroff(win, COLOUR_ROOMINFO);
    wprintw(win, "%s\n", message);

    // currently in groupchat window
    if (win_index == current_index) {
        status_bar_active(win_index);
        dirty = TRUE;

    // not currenlty on groupchat window
    } else {
        status_bar_new(win_index);
    }
}

void
cons_show_login_success(ProfAccount *account)
{
    _win_show_time(console->win, '-');
    wprintw(console->win, "%s logged in successfully, ", account->jid);

    resource_presence_t presence = accounts_get_login_presence(account->name);
    const char *presence_str = string_from_resource_presence(presence);

    _presence_colour_on(console->win, presence_str);
    wprintw(console->win, "%s", presence_str);
    _presence_colour_off(console->win, presence_str);
    wprintw(console->win, " (priority %d)",
        accounts_get_priority_for_presence_type(account->name, presence));
    wprintw(console->win, ".\n");
}


void
cons_show_wins(void)
{
    int i = 0;
    int count = 0;

    cons_show("");
    cons_show("Active windows:");
    _win_show_time(console->win, '-');
    wprintw(console->win, "1: Console\n");

    for (i = 1; i < NUM_WINS; i++) {
        if (windows[i] != NULL) {
            count++;
        }
    }

    if (count != 0) {
        for (i = 1; i < NUM_WINS; i++) {
            if (windows[i] != NULL) {
                ProfWin *window = windows[i];
                _win_show_time(console->win, '-');

                switch (window->type)
                {
                    case WIN_CHAT:
                        wprintw(console->win, "%d: chat %s", i + 1, window->from);
                        PContact contact = contact_list_get_contact(window->from);

                        if (contact != NULL) {
                            if (p_contact_name(contact) != NULL) {
                                wprintw(console->win, " (%s)", p_contact_name(contact));
                            }
                            wprintw(console->win, " - %s", p_contact_presence(contact));
                        }

                        if (window->unread > 0) {
                            wprintw(console->win, ", %d unread", window->unread);
                        }

                        break;

                    case WIN_PRIVATE:
                        wprintw(console->win, "%d: private %s", i + 1, window->from);

                        if (window->unread > 0) {
                            wprintw(console->win, ", %d unread", window->unread);
                        }

                        break;

                    case WIN_MUC:
                        wprintw(console->win, "%d: room %s", i + 1, window->from);

                        if (window->unread > 0) {
                            wprintw(console->win, ", %d unread", window->unread);
                        }

                        break;

                    default:
                        break;
                }

                wprintw(console->win, "\n");
            }
        }
    }

    cons_show("");
}

void
cons_show_info(PContact pcontact)
{
    _win_show_info(console->win, pcontact);

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_show_caps(const char * const contact, Resource *resource)
{
    WINDOW *win = console->win;
    cons_show("");
    const char *resource_presence = string_from_resource_presence(resource->presence);
    _win_show_time(win, '-');
    _presence_colour_on(win, resource_presence);
    wprintw(win, "%s", contact);
    _presence_colour_off(win, resource_presence);
    wprintw(win, ":\n");

    if (resource->caps_str != NULL) {
        log_debug("Getting caps, caps_str: %s", resource->caps_str);
        Capabilities *caps = caps_get(resource->caps_str);
        if (caps != NULL) {
            // show identity
            if ((caps->category != NULL) || (caps->type != NULL) || (caps->name != NULL)) {
                _win_show_time(win, '-');
                wprintw(win, "Identity: ");
                if (caps->name != NULL) {
                    wprintw(win, "%s", caps->name);
                    if ((caps->category != NULL) || (caps->type != NULL)) {
                        wprintw(win, " ");
                    }
                }
                if (caps->type != NULL) {
                    wprintw(win, "%s", caps->type);
                    if (caps->category != NULL) {
                        wprintw(win, " ");
                    }
                }
                if (caps->category != NULL) {
                    wprintw(win, "%s", caps->category);
                }
                wprintw(win, "\n");
            }
            if (caps->software != NULL) {
                _win_show_time(win, '-');
                wprintw(win, "Software: %s", caps->software);
            }
            if (caps->software_version != NULL) {
                wprintw(win, ", %s", caps->software_version);
            }
            if ((caps->software != NULL) || (caps->software_version != NULL)) {
                wprintw(win, "\n");
            }
            if (caps->os != NULL) {
                _win_show_time(win, '-');
                wprintw(win, "OS: %s", caps->os);
            }
            if (caps->os_version != NULL) {
                wprintw(win, ", %s", caps->os_version);
            }
            if ((caps->os != NULL) || (caps->os_version != NULL)) {
                wprintw(win, "\n");
            }

            if (caps->features != NULL) {
                _win_show_time(win, '-');
                wprintw(win, "Features:\n");
                GSList *feature = caps->features;
                while (feature != NULL) {
                    _win_show_time(win, '-');
                    wprintw(win, "  %s\n", feature->data);
                    feature = g_slist_next(feature);
                }
            }
        }
    } else {
        log_debug("No caps string found in resource");
    }

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_show_software_version(const char * const jid, const char * const  presence,
    const char * const name, const char * const version, const char * const os)
{
    if ((name != NULL) || (version != NULL) || (os != NULL)) {
        cons_show("");
        _win_show_time(console->win, '-');
        _presence_colour_on(console->win, presence);
        wprintw(console->win, "%s", jid);
        _presence_colour_off(console->win, presence);
        wprintw(console->win, ":\n");
    }
    if (name != NULL) {
        cons_show("Name    : %s", name);
    }
    if (version != NULL) {
        cons_show("Version : %s", version);
    }
    if (os != NULL) {
        cons_show("OS      : %s", os);
    }
}

void
cons_show_room_list(GSList *rooms, const char * const conference_node)
{
    if ((rooms != NULL) && (g_slist_length(rooms) > 0)) {
        cons_show("Chat rooms at %s:", conference_node);
        while (rooms != NULL) {
            DiscoItem *room = rooms->data;
            _win_show_time(console->win, '-');
            wprintw(console->win, "  %s", room->jid);
            if (room->name != NULL) {
                wprintw(console->win, ", (%s)", room->name);
            }
            wprintw(console->win, "\n");
            rooms = g_slist_next(rooms);
        }
    } else {
        cons_show("No chat rooms at %s", conference_node);
    }
}

void
cons_show_disco_info(const char *jid, GSList *identities, GSList *features)
{
    if (((identities != NULL) && (g_slist_length(identities) > 0)) ||
        ((features != NULL) && (g_slist_length(features) > 0))) {
        cons_show("");
        cons_show("Service disovery info for %s", jid);

        if (identities != NULL) {
            cons_show("  Identities");
        }
        while (identities != NULL) {
            DiscoIdentity *identity = identities->data;  // anme trpe, cat
            GString *identity_str = g_string_new("    ");
            if (identity->name != NULL) {
                identity_str = g_string_append(identity_str, strdup(identity->name));
                identity_str = g_string_append(identity_str, " ");
            }
            if (identity->type != NULL) {
                identity_str = g_string_append(identity_str, strdup(identity->type));
                identity_str = g_string_append(identity_str, " ");
            }
            if (identity->category != NULL) {
                identity_str = g_string_append(identity_str, strdup(identity->category));
            }
            cons_show(identity_str->str);
            g_string_free(identity_str, FALSE);
            identities = g_slist_next(identities);
        }

        if (features != NULL) {
            cons_show("  Features:");
        }
        while (features != NULL) {
            cons_show("    %s", features->data);
            features = g_slist_next(features);
        }
    }
}

void
cons_show_disco_items(GSList *items, const char * const jid)
{
    if ((items != NULL) && (g_slist_length(items) > 0)) {
        cons_show("");
        cons_show("Service discovery items for %s:", jid);
        while (items != NULL) {
            DiscoItem *item = items->data;
            _win_show_time(console->win, '-');
            wprintw(console->win, "  %s", item->jid);
            if (item->name != NULL) {
                wprintw(console->win, ", (%s)", item->name);
            }
            wprintw(console->win, "\n");
            items = g_slist_next(items);
        }
    } else {
        cons_show("");
        cons_show("No service discovery items for %s", jid);
    }
}

void
cons_show_status(const char * const contact)
{
    PContact pcontact = contact_list_get_contact(contact);

    if (pcontact != NULL) {
        _win_show_contact(console, pcontact);
    } else {
        cons_show("No such contact \"%s\" in roster.", contact);
    }
}

void
cons_show_room_invite(const char * const invitor, const char * const room,
    const char * const reason)
{
    cons_show("");
    _win_show_time(console->win, '-');
    wprintw(console->win, "%s has invited you to join %s", invitor, room);
    if (reason != NULL) {
        wprintw(console->win, ", \"%s\"", reason);
    }
    wprintw(console->win, "\n");
    cons_show("Type \"/join %s\" to join the room", room);
}

void
cons_show_account_list(gchar **accounts)
{
    int size = g_strv_length(accounts);
    if (size > 0) {
        cons_show("Accounts:");
        int i = 0;
        for (i = 0; i < size; i++) {
            if ((jabber_get_connection_status() == JABBER_CONNECTED) &&
                    (g_strcmp0(jabber_get_account_name(), accounts[i]) == 0)) {
                resource_presence_t presence = accounts_get_last_presence(accounts[i]);
                _win_show_time(console->win, '-');
                _presence_colour_on(console->win, string_from_resource_presence(presence));
                wprintw(console->win, "%s\n", accounts[i]);
                _presence_colour_off(console->win, string_from_resource_presence(presence));
            } else {
                cons_show(accounts[i]);
            }
        }
        cons_show("");
    } else {
        cons_show("No accounts created yet.");
        cons_show("");
    }
}

void
cons_show_account(ProfAccount *account)
{
    cons_show("");
    cons_show("Account %s:", account->name);
    if (account->enabled) {
        cons_show   ("enabled        : TRUE");
    } else {
        cons_show   ("enabled        : FALSE");
    }
    cons_show       ("jid            : %s", account->jid);
    if (account->resource != NULL) {
        cons_show   ("resource       : %s", account->resource);
    }
    if (account->server != NULL) {
        cons_show   ("server         : %s", account->server);
    }
    if (account->last_presence != NULL) {
        cons_show   ("Last presence  : %s", account->last_presence);
    }
    if (account->login_presence != NULL) {
        cons_show   ("Login presence : %s", account->login_presence);
    }
    cons_show       ("Priority       : chat:%d, online:%d, away:%d, xa:%d, dnd:%d",
        account->priority_chat, account->priority_online, account->priority_away,
        account->priority_xa, account->priority_dnd);

    if ((jabber_get_connection_status() == JABBER_CONNECTED) &&
            (g_strcmp0(jabber_get_account_name(), account->name) == 0)) {
        GList *resources = jabber_get_available_resources();
        GList *ordered_resources = NULL;

        WINDOW *win = console->win;
        if (resources != NULL) {
            _win_show_time(win, '-');
            wprintw(win, "Resources:\n");

            // sort in order of availabiltiy
            while (resources != NULL) {
                Resource *resource = resources->data;
                ordered_resources = g_list_insert_sorted(ordered_resources,
                    resource, (GCompareFunc)resource_compare_availability);
                resources = g_list_next(resources);
            }
        }

        while (ordered_resources != NULL) {
            Resource *resource = ordered_resources->data;
            const char *resource_presence = string_from_resource_presence(resource->presence);
            _win_show_time(win, '-');
            _presence_colour_on(win, resource_presence);
            wprintw(win, "  %s (%d), %s", resource->name, resource->priority, resource_presence);
            if (resource->status != NULL) {
                wprintw(win, ", \"%s\"", resource->status);
            }
            wprintw(win, "\n");
            _presence_colour_off(win, resource_presence);

            if (resource->caps_str != NULL) {
                Capabilities *caps = caps_get(resource->caps_str);
                if (caps != NULL) {
                    // show identity
                    if ((caps->category != NULL) || (caps->type != NULL) || (caps->name != NULL)) {
                        _win_show_time(win, '-');
                        wprintw(win, "    Identity: ");
                        if (caps->name != NULL) {
                            wprintw(win, "%s", caps->name);
                            if ((caps->category != NULL) || (caps->type != NULL)) {
                                wprintw(win, " ");
                            }
                        }
                        if (caps->type != NULL) {
                            wprintw(win, "%s", caps->type);
                            if (caps->category != NULL) {
                                wprintw(win, " ");
                            }
                        }
                        if (caps->category != NULL) {
                            wprintw(win, "%s", caps->category);
                        }
                        wprintw(win, "\n");
                    }
                    if (caps->software != NULL) {
                        _win_show_time(win, '-');
                        wprintw(win, "    Software: %s", caps->software);
                    }
                    if (caps->software_version != NULL) {
                        wprintw(win, ", %s", caps->software_version);
                    }
                    if ((caps->software != NULL) || (caps->software_version != NULL)) {
                        wprintw(win, "\n");
                    }
                    if (caps->os != NULL) {
                        _win_show_time(win, '-');
                        wprintw(win, "    OS: %s", caps->os);
                    }
                    if (caps->os_version != NULL) {
                        wprintw(win, ", %s", caps->os_version);
                    }
                    if ((caps->os != NULL) || (caps->os_version != NULL)) {
                        wprintw(win, "\n");
                    }
                }
            }

            ordered_resources = g_list_next(ordered_resources);
        }
    }
}

void
win_show_status(void)
{
    char *recipient = win_current_get_recipient();
    PContact pcontact = contact_list_get_contact(recipient);

    if (pcontact != NULL) {
        _win_show_contact(current, pcontact);
    } else {
        win_current_show("Error getting contact info.");
    }
}

void
win_private_show_status(void)
{
    Jid *jid = jid_create(win_current_get_recipient());

    PContact pcontact = muc_get_participant(jid->barejid, jid->resourcepart);

    if (pcontact != NULL) {
        _win_show_contact(current, pcontact);
    } else {
        win_current_show("Error getting contact info.");
    }

    jid_destroy(jid);
}

void
win_room_show_status(const char * const contact)
{
    PContact pcontact = muc_get_participant(win_current_get_recipient(), contact);

    if (pcontact != NULL) {
        _win_show_contact(current, pcontact);
    } else {
        win_current_show("No such participant \"%s\" in room.", contact);
    }
}

void
cons_show_ui_prefs(void)
{
    cons_show("UI preferences:");
    cons_show("");

    gchar *theme = prefs_get_string(PREF_THEME);
    if (theme == NULL) {
        cons_show("Theme (/theme)               : default");
    } else {
        cons_show("Theme (/theme)               : %s", theme);
    }

    if (prefs_get_boolean(PREF_BEEP))
        cons_show("Terminal beep (/beep)        : ON");
    else
        cons_show("Terminal beep (/beep)        : OFF");

    if (prefs_get_boolean(PREF_FLASH))
        cons_show("Terminal flash (/flash)      : ON");
    else
        cons_show("Terminal flash (/flash)      : OFF");

    if (prefs_get_boolean(PREF_INTYPE))
        cons_show("Show typing (/intype)        : ON");
    else
        cons_show("Show typing (/intype)        : OFF");

    if (prefs_get_boolean(PREF_SPLASH))
        cons_show("Splash screen (/splash)      : ON");
    else
        cons_show("Splash screen (/splash)      : OFF");

    if (prefs_get_boolean(PREF_HISTORY))
        cons_show("Chat history (/history)      : ON");
    else
        cons_show("Chat history (/history)      : OFF");

    if (prefs_get_boolean(PREF_VERCHECK))
        cons_show("Version checking (/vercheck) : ON");
    else
        cons_show("Version checking (/vercheck) : OFF");

    if (prefs_get_boolean(PREF_MOUSE))
        cons_show("Mouse handling (/mouse)      : ON");
    else
        cons_show("Mouse handling (/mouse)      : OFF");

    if (prefs_get_boolean(PREF_STATUSES))
        cons_show("Status (/statuses)           : ON");
    else
        cons_show("Status (/statuses)           : OFF");
}

void
cons_show_desktop_prefs(void)
{
    cons_show("Desktop notification preferences:");
    cons_show("");

    if (prefs_get_boolean(PREF_NOTIFY_MESSAGE))
        cons_show("Messages (/notify message)       : ON");
    else
        cons_show("Messages (/notify message)       : OFF");

    if (prefs_get_boolean(PREF_NOTIFY_TYPING))
        cons_show("Composing (/notify typing)       : ON");
    else
        cons_show("Composing (/notify typing)       : OFF");

    gint remind_period = prefs_get_notify_remind();
    if (remind_period == 0) {
        cons_show("Reminder period (/notify remind) : OFF");
    } else if (remind_period == 1) {
        cons_show("Reminder period (/notify remind) : 1 second");
    } else {
        cons_show("Reminder period (/notify remind) : %d seconds", remind_period);
    }
}

void
cons_show_chat_prefs(void)
{
    cons_show("Chat preferences:");
    cons_show("");

    if (prefs_get_boolean(PREF_STATES))
        cons_show("Send chat states (/states) : ON");
    else
        cons_show("Send chat states (/states) : OFF");

    if (prefs_get_boolean(PREF_OUTTYPE))
        cons_show("Send composing (/outtype)  : ON");
    else
        cons_show("Send composing (/outtype)  : OFF");

    gint gone_time = prefs_get_gone();
    if (gone_time == 0) {
        cons_show("Leave conversation (/gone) : OFF");
    } else if (gone_time == 1) {
        cons_show("Leave conversation (/gone) : 1 minute");
    } else {
        cons_show("Leave conversation (/gone) : %d minutes", gone_time);
    }
}

void
cons_show_log_prefs(void)
{
    cons_show("Logging preferences:");
    cons_show("");

    cons_show("Max log size (/log maxsize) : %d bytes", prefs_get_max_log_size());

    if (prefs_get_boolean(PREF_CHLOG))
        cons_show("Chat logging (/chlog)       : ON");
    else
        cons_show("Chat logging (/chlog)       : OFF");
}

void
cons_show_presence_prefs(void)
{
    cons_show("Presence preferences:");
    cons_show("");

    if (strcmp(prefs_get_string(PREF_AUTOAWAY_MODE), "off") == 0) {
        cons_show("Autoaway (/autoaway mode)            : OFF");
    } else {
        cons_show("Autoaway (/autoaway mode)            : %s", prefs_get_string(PREF_AUTOAWAY_MODE));
    }

    cons_show("Autoaway minutes (/autoaway time)    : %d minutes", prefs_get_autoaway_time());

    if ((prefs_get_string(PREF_AUTOAWAY_MESSAGE) == NULL) ||
            (strcmp(prefs_get_string(PREF_AUTOAWAY_MESSAGE), "") == 0)) {
        cons_show("Autoaway message (/autoaway message) : OFF");
    } else {
        cons_show("Autoaway message (/autoaway message) : \"%s\"", prefs_get_string(PREF_AUTOAWAY_MESSAGE));
    }

    if (prefs_get_boolean(PREF_AUTOAWAY_CHECK)) {
        cons_show("Autoaway check (/autoaway check)     : ON");
    } else {
        cons_show("Autoaway check (/autoaway check)     : OFF");
    }
}

void
cons_show_connection_prefs(void)
{
    cons_show("Connection preferences:");
    cons_show("");

    gint reconnect_interval = prefs_get_reconnect();
    if (reconnect_interval == 0) {
        cons_show("Reconnect interval (/reconnect) : OFF");
    } else if (reconnect_interval == 1) {
        cons_show("Reconnect interval (/reconnect) : 1 second");
    } else {
        cons_show("Reconnect interval (/reconnect) : %d seconds", reconnect_interval);
    }

    gint autoping_interval = prefs_get_autoping();
    if (autoping_interval == 0) {
        cons_show("Autoping interval (/autoping)   : OFF");
    } else if (autoping_interval == 1) {
        cons_show("Autoping interval (/autoping)   : 1 second");
    } else {
        cons_show("Autoping interval (/autoping)   : %d seconds", autoping_interval);
    }
}

void
cons_show_themes(GSList *themes)
{
    cons_show("");

    if (themes == NULL) {
        cons_show("No available themes.");
    } else {
        cons_show("Available themes:");
        while (themes != NULL) {
            cons_show(themes->data);
            themes = g_slist_next(themes);
        }
    }
}

void
cons_prefs(void)
{
    cons_show("");
    cons_show_ui_prefs();
    cons_show("");
    cons_show_desktop_prefs();
    cons_show("");
    cons_show_chat_prefs();
    cons_show("");
    cons_show_log_prefs();
    cons_show("");
    cons_show_presence_prefs();
    cons_show("");
    cons_show_connection_prefs();
    cons_show("");

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

static void
_cons_show_basic_help(void)
{
    cons_show("");

    GSList *basic_helpers = cmd_get_basic_help();
    while (basic_helpers != NULL) {
        struct cmd_help_t *help = (struct cmd_help_t *)basic_helpers->data;
        cons_show("%-30s: %s", help->usage, help->short_help);
        basic_helpers = g_slist_next(basic_helpers);
    }

    cons_show("");
}

void
cons_help(void)
{
    cons_show("");
    cons_show("Choose a help option:");
    cons_show("");
    cons_show("/help list       - List all commands.");
    cons_show("/help basic      - Summary of basic usage commands.");
    cons_show("/help presence   - Summary of online status change commands.");
    cons_show("/help settings   - Summary of commands for changing Profanity settings.");
    cons_show("/help navigation - How to navigate around Profanity.");
    cons_show("/help [command]  - Detailed help on a specific command.");
    cons_show("");

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_basic_help(void)
{
    cons_show("");
    cons_show("Basic Commands:");
    _cons_show_basic_help();

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_settings_help(void)
{
    cons_show("");
    cons_show("Settings:");
    cons_show("");

    GSList *settings_helpers = cmd_get_settings_help();
    while (settings_helpers != NULL) {
        struct cmd_help_t *help = (struct cmd_help_t *)settings_helpers->data;
        cons_show("%-27s: %s", help->usage, help->short_help);
        settings_helpers = g_slist_next(settings_helpers);
    }

    cons_show("");

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_presence_help(void)
{
    cons_show("");
    cons_show("Presence changes:");
    cons_show("");

    GSList *presence_helpers = cmd_get_presence_help();
    while (presence_helpers != NULL) {
        struct cmd_help_t *help = (struct cmd_help_t *)presence_helpers->data;
        cons_show("%-25s: %s", help->usage, help->short_help);
        presence_helpers = g_slist_next(presence_helpers);
    }

    cons_show("");

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_navigation_help(void)
{
    cons_show("");
    cons_show("Navigation:");
    cons_show("");
    cons_show("Alt-1                    : This console window.");
    cons_show("Alt-2..Alt-0             : Chat windows.");
    cons_show("F1                       : This console window.");
    cons_show("F2..F10                  : Chat windows.");
    cons_show("UP, DOWN                 : Navigate input history.");
    cons_show("LEFT, RIGHT, HOME, END   : Edit current input.");
    cons_show("ESC                      : Clear current input.");
    cons_show("TAB                      : Autocomplete command/recipient/login.");
    cons_show("PAGE UP, PAGE DOWN       : Page the main window.");
    cons_show("");

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_show_contacts(GSList *list)
{
    GSList *curr = list;

    while(curr) {
        PContact contact = curr->data;
        if (strcmp(p_contact_subscription(contact), "none") != 0) {
            _win_show_contact(console, contact);
        }
        curr = g_slist_next(curr);
    }
}

void
cons_bad_show(const char * const msg, ...)
{
    va_list arg;
    va_start(arg, msg);
    GString *fmt_msg = g_string_new(NULL);
    g_string_vprintf(fmt_msg, msg, arg);
    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_ERROR);
    wprintw(console->win, "%s\n", fmt_msg->str);
    wattroff(console->win, COLOUR_ERROR);
    g_string_free(fmt_msg, TRUE);
    va_end(arg);

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_show_time(void)
{
    _win_show_time(console->win, '-');
}

void
cons_show(const char * const msg, ...)
{
    va_list arg;
    va_start(arg, msg);
    GString *fmt_msg = g_string_new(NULL);
    g_string_vprintf(fmt_msg, msg, arg);
    _win_show_time(console->win, '-');
    wprintw(console->win, "%s\n", fmt_msg->str);
    g_string_free(fmt_msg, TRUE);
    va_end(arg);

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_debug(const char * const msg, ...)
{
    if (strcmp(PACKAGE_STATUS, "development") == 0) {
        va_list arg;
        va_start(arg, msg);
        GString *fmt_msg = g_string_new(NULL);
        g_string_vprintf(fmt_msg, msg, arg);
        _win_show_time(console->win, '-');
        wprintw(console->win, "%s\n", fmt_msg->str);
        g_string_free(fmt_msg, TRUE);
        va_end(arg);

        if (current_index == 0) {
            dirty = TRUE;
        } else {
            status_bar_new(0);
        }

        win_current_page_off();
        ui_refresh();
    }
}

void
cons_show_word(const char * const word)
{
    wprintw(console->win, "%s", word);

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_bad_command(const char * const cmd)
{
    _win_show_time(console->win, '-');
    wprintw(console->win, "Unknown command: %s\n", cmd);

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_about(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    if (prefs_get_boolean(PREF_SPLASH)) {
        _cons_splash_logo();
    } else {
        _win_show_time(console->win, '-');

        if (strcmp(PACKAGE_STATUS, "development") == 0) {
            wprintw(console->win, "Welcome to Profanity, version %sdev\n", PACKAGE_VERSION);
        } else {
            wprintw(console->win, "Welcome to Profanity, version %s\n", PACKAGE_VERSION);
        }
    }

    _win_show_time(console->win, '-');
    wprintw(console->win, "Copyright (C) 2012, 2013 James Booth <%s>.\n", PACKAGE_BUGREPORT);
    _win_show_time(console->win, '-');
    wprintw(console->win, "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n");
    _win_show_time(console->win, '-');
    wprintw(console->win, "\n");
    _win_show_time(console->win, '-');
    wprintw(console->win, "This is free software; you are free to change and redistribute it.\n");
    _win_show_time(console->win, '-');
    wprintw(console->win, "There is NO WARRANTY, to the extent permitted by law.\n");
    _win_show_time(console->win, '-');
    wprintw(console->win, "\n");
    _win_show_time(console->win, '-');
    wprintw(console->win, "Type '/help' to show complete help.\n");
    _win_show_time(console->win, '-');
    wprintw(console->win, "\n");

    if (prefs_get_boolean(PREF_VERCHECK)) {
        cons_check_version(FALSE);
    }

    prefresh(console->win, 0, 0, 1, 0, rows-3, cols-1);

    if (current_index == 0) {
        dirty = TRUE;
    } else {
        status_bar_new(0);
    }
}

void
cons_check_version(gboolean not_available_msg)
{
    char *latest_release = release_get_latest();

    if (latest_release != NULL) {
        gboolean relase_valid = g_regex_match_simple("^\\d+\\.\\d+\\.\\d+$", latest_release, 0, 0);

        if (relase_valid) {
            if (_new_release(latest_release)) {
                _win_show_time(console->win, '-');
                wprintw(console->win, "A new version of Profanity is available: %s", latest_release);
                _win_show_time(console->win, '-');
                wprintw(console->win, "Check <http://www.profanity.im> for details.\n");
                free(latest_release);
                _win_show_time(console->win, '-');
                wprintw(console->win, "\n");
            } else {
                if (not_available_msg) {
                    cons_show("No new version available.");
                    cons_show("");
                }
            }

            if (current_index == 0) {
                dirty = TRUE;
            } else {
                status_bar_new(0);
            }
        }
    }
}

void
notify_remind(void)
{
    gint unread = _win_get_unread();
    if (unread > 0) {
        _notify_remind(unread);
    }
}

static void
_notify(const char * const message, int timeout,
    const char * const category)
{
#ifdef HAVE_LIBNOTIFY
    gboolean notify_initted = notify_is_initted();

    if (!notify_initted) {
        notify_initted = notify_init("Profanity");
    }

    if (notify_initted) {
        NotifyNotification *notification;
        notification = notify_notification_new("Profanity", message, NULL);
        notify_notification_set_timeout(notification, timeout);
        notify_notification_set_category(notification, category);
        notify_notification_set_urgency(notification, NOTIFY_URGENCY_NORMAL);

        GError *error = NULL;
        gboolean notify_success = notify_notification_show(notification, &error);

        if (!notify_success) {
            log_error("Error sending desktop notification:");
            log_error("  -> Message : %s", message);
            log_error("  -> Error   : %s", error->message);
        }
    } else {
        log_error("Libnotify initialisation error.");
    }
#endif
#ifdef PLATFORM_CYGWIN
    NOTIFYICONDATA nid;
    nid.cbSize = sizeof(NOTIFYICONDATA);
    //nid.hWnd = hWnd;
    nid.uID = 100;
    nid.uVersion = NOTIFYICON_VERSION;
    //nid.uCallbackMessage = WM_MYMESSAGE;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    strcpy(nid.szTip, "Tray Icon");
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    Shell_NotifyIcon(NIM_ADD, &nid);

    // For a Ballon Tip
    nid.uFlags = NIF_INFO;
    strcpy(nid.szInfoTitle, "Profanity"); // Title
    strcpy(nid.szInfo, message); // Copy Tip
    nid.uTimeout = timeout;  // 3 Seconds
    nid.dwInfoFlags = NIIF_INFO;

    Shell_NotifyIcon(NIM_MODIFY, &nid);
#endif
}

static void
_notify_remind(gint unread)
{
    char message[20];
    if (unread == 1) {
        sprintf(message, "1 unread message");
    } else {
        snprintf(message, sizeof(message), "%d unread messages", unread);
    }

    _notify(message, 5000, "Incoming message");
}

static void
_notify_message(const char * const short_from)
{
    char message[strlen(short_from) + 1 + 10];
    sprintf(message, "%s: message.", short_from);

    _notify(message, 10000, "Incoming message");
}

static void
_notify_typing(const char * const from)
{
    char message[strlen(from) + 1 + 11];
    sprintf(message, "%s: typing...", from);

    _notify(message, 10000, "Incoming message");
}

static void
_create_windows(void)
{
    int cols = getmaxx(stdscr);
    max_cols = cols;
    windows[0] = window_create(CONS_WIN_TITLE, cols, WIN_CONSOLE);
    console = windows[0];
    current = console;
    cons_about();
}

static gboolean
_new_release(char *found_version)
{
    int curr_maj, curr_min, curr_patch, found_maj, found_min, found_patch;

    int parse_curr = sscanf(PACKAGE_VERSION, "%d.%d.%d", &curr_maj, &curr_min,
        &curr_patch);
    int parse_found = sscanf(found_version, "%d.%d.%d", &found_maj, &found_min,
        &found_patch);

    if (parse_found == 3 && parse_curr == 3) {
        if (found_maj > curr_maj) {
            return TRUE;
        } else if (found_maj == curr_maj && found_min > curr_min) {
            return TRUE;
        } else if (found_maj == curr_maj && found_min == curr_min
                                        && found_patch > curr_patch) {
            return TRUE;
        } else {
            return FALSE;
        }
    } else {
        return FALSE;
    }
}

static void
_cons_splash_logo(void)
{
    _win_show_time(console->win, '-');
    wprintw(console->win, "Welcome to\n");

    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_SPLASH);
    wprintw(console->win, "                   ___            _           \n");
    wattroff(console->win, COLOUR_SPLASH);

    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_SPLASH);
    wprintw(console->win, "                  / __)          (_)_         \n");
    wattroff(console->win, COLOUR_SPLASH);

    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_SPLASH);
    wprintw(console->win, " ____   ____ ___ | |__ ____ ____  _| |_ _   _ \n");
    wattroff(console->win, COLOUR_SPLASH);

    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_SPLASH);
    wprintw(console->win, "|  _ \\ / ___) _ \\|  __) _  |  _ \\| |  _) | | |\n");
    wattroff(console->win, COLOUR_SPLASH);

    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_SPLASH);
    wprintw(console->win, "| | | | |  | |_| | | ( ( | | | | | | |_| |_| |\n");
    wattroff(console->win, COLOUR_SPLASH);

    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_SPLASH);
    wprintw(console->win, "| ||_/|_|   \\___/|_|  \\_||_|_| |_|_|\\___)__  |\n");
    wattroff(console->win, COLOUR_SPLASH);

    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_SPLASH);
    wprintw(console->win, "|_|                                    (____/ \n");
    wattroff(console->win, COLOUR_SPLASH);

    _win_show_time(console->win, '-');
    wprintw(console->win, "\n");
    _win_show_time(console->win, '-');
    if (strcmp(PACKAGE_STATUS, "development") == 0) {
        wprintw(console->win, "Version %sdev\n", PACKAGE_VERSION);
    } else {
        wprintw(console->win, "Version %s\n", PACKAGE_VERSION);
    }
}

static int
_find_prof_win_index(const char * const contact)
{
    int i;
    for (i = 1; i < NUM_WINS; i++) {
        if ((windows[i] != NULL) && (strcmp(windows[i]->from, contact) == 0)) {
            break;
        }
    }

    return i;
}

static int
_new_prof_win(const char * const contact, win_type_t type)
{
    int i;
    for (i = 1; i < NUM_WINS; i++) {
        if (windows[i] == NULL) {
            break;
        }
    }

    if (i != NUM_WINS) {
        int cols = getmaxx(stdscr);
        windows[i] = window_create(contact, cols, type);
        return i;
    } else {
        return 0;
    }
}

static void
_win_show_time(WINDOW *win, char showchar)
{
    GDateTime *time = g_date_time_new_now_local();
    gchar *date_fmt = g_date_time_format(time, "%H:%M:%S");
    wattron(win, COLOUR_TIME);
    wprintw(win, "%s %c ", date_fmt, showchar);
    wattroff(win, COLOUR_TIME);
    g_date_time_unref(time);
    g_free(date_fmt);
}

static void
_win_show_user(WINDOW *win, const char * const user, const int colour)
{
    if (colour)
        wattron(win, COLOUR_THEM);
    else
        wattron(win, COLOUR_ME);
    wprintw(win, "%s: ", user);
    if (colour)
        wattroff(win, COLOUR_THEM);
    else
        wattroff(win, COLOUR_ME);
}

static void
_win_show_message(WINDOW *win, const char * const message)
{
    wprintw(win, "%s\n", message);
}

static void
_win_show_error_msg(WINDOW *win, const char * const message)
{
    wattron(win, COLOUR_ERROR);
    wprintw(win, "%s\n", message);
    wattroff(win, COLOUR_ERROR);
}

static void
_current_window_refresh(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    prefresh(current->win, current->y_pos, 0, 1, 0, rows-3, cols-1);
}

void
_win_resize_all(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    // only make the pads bigger, to avoid data loss on cropping
    if (cols > max_cols) {
        max_cols = cols;

        int i;
        for (i = 0; i < NUM_WINS; i++) {
            if (windows[i] != NULL) {
                wresize(windows[i]->win, PAD_SIZE, cols);
            }
        }
    }

    prefresh(current->win, current->y_pos, 0, 1, 0, rows-3, cols-1);
}

static void
_presence_colour_on(WINDOW *win, const char * const presence)
{
    if (g_strcmp0(presence, "online") == 0) {
        wattron(win, COLOUR_ONLINE);
    } else if (g_strcmp0(presence, "away") == 0) {
        wattron(win, COLOUR_AWAY);
    } else if (g_strcmp0(presence, "chat") == 0) {
        wattron(win, COLOUR_CHAT);
    } else if (g_strcmp0(presence, "dnd") == 0) {
        wattron(win, COLOUR_DND);
    } else if (g_strcmp0(presence, "xa") == 0) {
        wattron(win, COLOUR_XA);
    } else {
        wattron(win, COLOUR_OFFLINE);
    }
}

static void
_presence_colour_off(WINDOW *win, const char * const presence)
{
    if (g_strcmp0(presence, "online") == 0) {
        wattroff(win, COLOUR_ONLINE);
    } else if (g_strcmp0(presence, "away") == 0) {
        wattroff(win, COLOUR_AWAY);
    } else if (g_strcmp0(presence, "chat") == 0) {
        wattroff(win, COLOUR_CHAT);
    } else if (g_strcmp0(presence, "dnd") == 0) {
        wattroff(win, COLOUR_DND);
    } else if (g_strcmp0(presence, "xa") == 0) {
        wattroff(win, COLOUR_XA);
    } else {
        wattroff(win, COLOUR_OFFLINE);
    }
}

static void
_show_status_string(WINDOW *win, const char * const from,
    const char * const show, const char * const status,
    GDateTime *last_activity, const char * const pre,
    const char * const default_show)
{
    if (!prefs_get_boolean(PREF_STATUSES))
        return;

    _win_show_time(win, '-');

    if (show != NULL) {
        if (strcmp(show, "away") == 0) {
            wattron(win, COLOUR_AWAY);
        } else if (strcmp(show, "chat") == 0) {
            wattron(win, COLOUR_CHAT);
        } else if (strcmp(show, "dnd") == 0) {
            wattron(win, COLOUR_DND);
        } else if (strcmp(show, "xa") == 0) {
            wattron(win, COLOUR_XA);
        } else if (strcmp(show, "online") == 0) {
            wattron(win, COLOUR_ONLINE);
        } else {
            wattron(win, COLOUR_OFFLINE);
        }
    } else if (strcmp(default_show, "online") == 0) {
        wattron(win, COLOUR_ONLINE);
    } else {
        wattron(win, COLOUR_OFFLINE);
    }

    wprintw(win, "%s %s", pre, from);

    if (show != NULL)
        wprintw(win, " is %s", show);
    else
        wprintw(win, " is %s", default_show);

    if (last_activity != NULL) {
        GDateTime *now = g_date_time_new_now_local();
        GTimeSpan span = g_date_time_difference(now, last_activity);

        wprintw(win, ", idle ");

        int hours = span / G_TIME_SPAN_HOUR;
        span = span - hours * G_TIME_SPAN_HOUR;
        if (hours > 0) {
            wprintw(win, "%dh", hours);
        }

        int minutes = span / G_TIME_SPAN_MINUTE;
        span = span - minutes * G_TIME_SPAN_MINUTE;
        wprintw(win, "%dm", minutes);

        int seconds = span / G_TIME_SPAN_SECOND;
        wprintw(win, "%ds", seconds);
    }

    if (status != NULL)
        wprintw(win, ", \"%s\"", status);

    wprintw(win, "\n");

    if (show != NULL) {
        if (strcmp(show, "away") == 0) {
            wattroff(win, COLOUR_AWAY);
        } else if (strcmp(show, "chat") == 0) {
            wattroff(win, COLOUR_CHAT);
        } else if (strcmp(show, "dnd") == 0) {
            wattroff(win, COLOUR_DND);
        } else if (strcmp(show, "xa") == 0) {
            wattroff(win, COLOUR_XA);
        } else if (strcmp(show, "online") == 0) {
            wattroff(win, COLOUR_ONLINE);
        } else {
            wattroff(win, COLOUR_OFFLINE);
        }
    } else if (strcmp(default_show, "online") == 0) {
        wattroff(win, COLOUR_ONLINE);
    } else {
        wattroff(win, COLOUR_OFFLINE);
    }
}

static void
_cons_show_typing(const char * const short_from)
{
    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_TYPING);
    wprintw(console->win, "!! %s is typing a message...\n", short_from);
    wattroff(console->win, COLOUR_TYPING);
}

static void
_cons_show_incoming_message(const char * const short_from, const int win_index)
{
    _win_show_time(console->win, '-');
    wattron(console->win, COLOUR_INCOMING);
    wprintw(console->win, "<< incoming from %s (%d)\n", short_from, win_index + 1);
    wattroff(console->win, COLOUR_INCOMING);
}

static void
_win_show_contact(ProfWin *window, PContact contact)
{
    const char *barejid = p_contact_barejid(contact);
    const char *name = p_contact_name(contact);
    const char *presence = p_contact_presence(contact);
    const char *status = p_contact_status(contact);
    GDateTime *last_activity = p_contact_last_activity(contact);

    _win_show_time(window->win, '-');
    _presence_colour_on(window->win, presence);
    wprintw(window->win, "%s", barejid);

    if (name != NULL) {
        wprintw(window->win, " (%s)", name);
    }

    wprintw(window->win, " is %s", presence);

    if (last_activity != NULL) {
        GDateTime *now = g_date_time_new_now_local();
        GTimeSpan span = g_date_time_difference(now, last_activity);

        wprintw(window->win, ", idle ");

        int hours = span / G_TIME_SPAN_HOUR;
        span = span - hours * G_TIME_SPAN_HOUR;
        if (hours > 0) {
            wprintw(window->win, "%dh", hours);
        }

        int minutes = span / G_TIME_SPAN_MINUTE;
        span = span - minutes * G_TIME_SPAN_MINUTE;
        wprintw(window->win, "%dm", minutes);

        int seconds = span / G_TIME_SPAN_SECOND;
        wprintw(window->win, "%ds", seconds);
    }

    if (status != NULL) {
        wprintw(window->win, ", \"%s\"", p_contact_status(contact));
    }

    wprintw(window->win, "\n");
    _presence_colour_off(window->win, presence);
}

static void
_win_handle_switch(const wint_t * const ch)
{
    if (*ch == KEY_F(1)) {
        ui_switch_win(0);
    } else if (*ch == KEY_F(2)) {
        ui_switch_win(1);
    } else if (*ch == KEY_F(3)) {
        ui_switch_win(2);
    } else if (*ch == KEY_F(4)) {
        ui_switch_win(3);
    } else if (*ch == KEY_F(5)) {
        ui_switch_win(4);
    } else if (*ch == KEY_F(6)) {
        ui_switch_win(5);
    } else if (*ch == KEY_F(7)) {
        ui_switch_win(6);
    } else if (*ch == KEY_F(8)) {
        ui_switch_win(7);
    } else if (*ch == KEY_F(9)) {
        ui_switch_win(8);
    } else if (*ch == KEY_F(10)) {
        ui_switch_win(9);
    }
}

static void
_win_handle_page(const wint_t * const ch)
{
    int rows = getmaxy(stdscr);
    int y = getcury(current->win);

    int page_space = rows - 4;
    int *page_start = &(current->y_pos);

    if (prefs_get_boolean(PREF_MOUSE)) {
        MEVENT mouse_event;

        if (*ch == KEY_MOUSE) {
            if (getmouse(&mouse_event) == OK) {

#ifdef PLATFORM_CYGWIN
                if (mouse_event.bstate & BUTTON5_PRESSED) { // mouse wheel down
#else
                if (mouse_event.bstate & BUTTON2_PRESSED) { // mouse wheel down
#endif
                    *page_start += 4;

                    // only got half a screen, show full screen
                    if ((y - (*page_start)) < page_space)
                        *page_start = y - page_space;

                    // went past end, show full screen
                    else if (*page_start >= y)
                        *page_start = y - page_space;

                    current->paged = 1;
                    dirty = TRUE;
                } else if (mouse_event.bstate & BUTTON4_PRESSED) { // mouse wheel up
                    *page_start -= 4;

                    // went past beginning, show first page
                    if (*page_start < 0)
                        *page_start = 0;

                    current->paged = 1;
                    dirty = TRUE;
                }
            }
        }
    }

    // page up
    if (*ch == KEY_PPAGE) {
        *page_start -= page_space;

        // went past beginning, show first page
        if (*page_start < 0)
            *page_start = 0;

        current->paged = 1;
        dirty = TRUE;

    // page down
    } else if (*ch == KEY_NPAGE) {
        *page_start += page_space;

        // only got half a screen, show full screen
        if ((y - (*page_start)) < page_space)
            *page_start = y - page_space;

        // went past end, show full screen
        else if (*page_start >= y)
            *page_start = y - page_space;

        current->paged = 1;
        dirty = TRUE;
    }
}

static gint
_win_get_unread(void)
{
    int i;
    gint result = 0;
    for (i = 0; i < NUM_WINS; i++) {
        if (windows[i] != NULL) {
            result += windows[i]->unread;
        }
    }
    return result;
}

static void
_win_show_history(WINDOW *win, int win_index, const char * const contact)
{
    if (!windows[win_index]->history_shown) {
        GSList *history = NULL;
        Jid *jid = jid_create(jabber_get_jid());
        history = chat_log_get_previous(jid->barejid, contact, history);
        jid_destroy(jid);
        while (history != NULL) {
            wprintw(win, "%s\n", history->data);
            history = g_slist_next(history);
        }
        windows[win_index]->history_shown = 1;

        g_slist_free_full(history, free);
    }
}

static void
_win_show_info(WINDOW *win, PContact pcontact)
{
    const char *barejid = p_contact_barejid(pcontact);
    const char *name = p_contact_name(pcontact);
    const char *presence = p_contact_presence(pcontact);
    const char *sub = p_contact_subscription(pcontact);
    GList *resources = p_contact_get_available_resources(pcontact);
    GList *ordered_resources = NULL;
    GDateTime *last_activity = p_contact_last_activity(pcontact);

    _win_show_time(win, '-');
    wprintw(win, "\n");
    _win_show_time(win, '-');
    _presence_colour_on(win, presence);
    wprintw(win, "%s", barejid);
    if (name != NULL) {
        wprintw(win, " (%s)", name);
    }
    _presence_colour_off(win, presence);
    wprintw(win, ":\n");

    if (sub != NULL) {
        _win_show_time(win, '-');
        wprintw(win, "Subscription: %s\n", sub);
    }

    if (last_activity != NULL) {
        GDateTime *now = g_date_time_new_now_local();
        GTimeSpan span = g_date_time_difference(now, last_activity);

        _win_show_time(win, '-');
        wprintw(win, "Last activity: ");

        int hours = span / G_TIME_SPAN_HOUR;
        span = span - hours * G_TIME_SPAN_HOUR;
        if (hours > 0) {
            wprintw(win, "%dh", hours);
        }

        int minutes = span / G_TIME_SPAN_MINUTE;
        span = span - minutes * G_TIME_SPAN_MINUTE;
        wprintw(win, "%dm", minutes);

        int seconds = span / G_TIME_SPAN_SECOND;
        wprintw(win, "%ds", seconds);

        wprintw(win, "\n");

        g_date_time_unref(now);
    }

    if (resources != NULL) {
        _win_show_time(win, '-');
        wprintw(win, "Resources:\n");

        // sort in order of availabiltiy
        while (resources != NULL) {
            Resource *resource = resources->data;
            ordered_resources = g_list_insert_sorted(ordered_resources,
                resource, (GCompareFunc)resource_compare_availability);
            resources = g_list_next(resources);
        }
    }

    while (ordered_resources != NULL) {
        Resource *resource = ordered_resources->data;
        const char *resource_presence = string_from_resource_presence(resource->presence);
        _win_show_time(win, '-');
        _presence_colour_on(win, resource_presence);
        wprintw(win, "  %s (%d), %s", resource->name, resource->priority, resource_presence);
        if (resource->status != NULL) {
            wprintw(win, ", \"%s\"", resource->status);
        }
        wprintw(win, "\n");
        _presence_colour_off(win, resource_presence);

        if (resource->caps_str != NULL) {
            Capabilities *caps = caps_get(resource->caps_str);
            if (caps != NULL) {
                // show identity
                if ((caps->category != NULL) || (caps->type != NULL) || (caps->name != NULL)) {
                    _win_show_time(win, '-');
                    wprintw(win, "    Identity: ");
                    if (caps->name != NULL) {
                        wprintw(win, "%s", caps->name);
                        if ((caps->category != NULL) || (caps->type != NULL)) {
                            wprintw(win, " ");
                        }
                    }
                    if (caps->type != NULL) {
                        wprintw(win, "%s", caps->type);
                        if (caps->category != NULL) {
                            wprintw(win, " ");
                        }
                    }
                    if (caps->category != NULL) {
                        wprintw(win, "%s", caps->category);
                    }
                    wprintw(win, "\n");
                }
                if (caps->software != NULL) {
                    _win_show_time(win, '-');
                    wprintw(win, "    Software: %s", caps->software);
                }
                if (caps->software_version != NULL) {
                    wprintw(win, ", %s", caps->software_version);
                }
                if ((caps->software != NULL) || (caps->software_version != NULL)) {
                    wprintw(win, "\n");
                }
                if (caps->os != NULL) {
                    _win_show_time(win, '-');
                    wprintw(win, "    OS: %s", caps->os);
                }
                if (caps->os_version != NULL) {
                    wprintw(win, ", %s", caps->os_version);
                }
                if ((caps->os != NULL) || (caps->os_version != NULL)) {
                    wprintw(win, "\n");
                }
            }
        }

        ordered_resources = g_list_next(ordered_resources);
    }
}

void
_set_current(int index)
{
    current_index = index;
    current = windows[current_index];
}

