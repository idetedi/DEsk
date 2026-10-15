#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <cairo.h>

struct wl_event_loop;

/* The screen lock's half that is not drawing (lock.c): who is at the desk,
 * their picture, checking their password, and logind's say in the matter. */

/* The name to put on the door: the real name from the password file (its
 * first comma-separated field), or the login name. */
void desk_lock_user(char *login, size_t nl, char *name, size_t nn);
/* Their picture, square-ish, or NULL: ~/.face (a PNG, or an SVG turned into
 * one), else AccountsService's. The caller owns it. */
cairo_surface_t *desk_lock_picture(void);

/* Check a password, on a thread of its own (PAM can take seconds, and
 * takes them on purpose when it is wrong): done(ok, data) is called on the
 * compositor's loop when it is decided. The password is copied and the
 * copy wiped. False if it could not even be started. */
bool desk_lock_check(struct wl_event_loop *loop, const char *password,
	void (*done)(bool ok, void *data), void *data);

/* logind, for a desk that is the session: told to lock (loginctl
 * lock-session, the machine about to sleep) and to unlock (loginctl
 * unlock-session); and told whether it is locked. Nothing without the
 * system bus, or when DEsk is not the session. */
bool desk_lock_watch(struct wl_event_loop *loop, void (*lock)(void *data),
	void (*unlock)(void *data), void *data);
void desk_lock_hint(bool locked);
/* The machine about to sleep waits for the door (logind's delay lock, held
 * while awake): pending from then until desk_lock_sleep_ready, which the
 * compositor calls once the door has been on every lit screen -- or until
 * a few seconds have gone by, whatever happens. */
bool desk_lock_sleep_pending(void);
void desk_lock_sleep_ready(void);
