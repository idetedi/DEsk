/* The screen lock, apart from its drawing (lock.h).
 *
 * The password is checked with PAM, as at login, under the service
 * "desk" (session/pam/desk, which includes "login"), or "login" itself
 * where that is not installed. On a thread: pam_unix waits a few seconds
 * after a wrong one on purpose, and the desk must not stop for it. Only a
 * DEsk that is not the session (nested, in a window, under test) may be
 * pointed at another PAM directory, DESK_LOCK_PAM_DIR: nobody at a locked
 * screen can set an environment variable, and a test cannot use anyone's
 * real password. */

/* explicit_bzero and pipe2 are glibc's and the BSDs', past POSIX */
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include "lock.h"
#include "session.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#ifdef DESK_HAVE_POST
#include <systemd/sd-bus.h>
#endif

/* ---- the password ---- */

/* Down the pipe from the thread to the loop: something said, and the end. */
enum { CHECK_SAID = 2, CHECK_DONE = 1 };

struct check {
	char user[64];
	char pass[512];
	char confdir[512];
	bool ok;
	int fd[2];
	struct wl_event_source *src;
	void (*done)(bool ok, void *data);
	void (*said)(const char *text, void *data);
	void *data;
	/* what PAM has said so far: its last two lines, joined by '\n' */
	pthread_mutex_t said_lock;
	char said_text[512];
};

static void check_tell(struct check *c, char what)
{
	while (write(c->fd[1], &what, 1) < 0 && errno == EINTR) {
		;
	}
}

/* A line PAM has to say -- "The account is locked due to 3 failed logins.",
 * "Place your finger on the fingerprint reader" -- kept for the door, the
 * line before it with it and anything older let go. */
static void check_said(struct check *c, const char *msg)
{
	if (!msg) {
		return;
	}
	char line[200];
	snprintf(line, sizeof line, "%s", msg);
	/* one message, one line: no breaks in it, none at its end */
	for (char *p = line; *p; p++) {
		if (*p == '\n' || *p == '\r' || *p == '\t') {
			*p = ' ';
		}
	}
	size_t n = strlen(line);
	while (n > 0 && line[n - 1] == ' ') {
		line[--n] = '\0';
	}
	if (!line[0]) {
		return;
	}
	pthread_mutex_lock(&c->said_lock);
	/* the last line there is now kept, as the first */
	char before[sizeof c->said_text] = "";
	if (c->said_text[0]) {
		const char *nl = strchr(c->said_text, '\n');
		const char *from = nl ? nl + 1 : c->said_text;
		memcpy(before, from, strlen(from) + 1);
	}
	size_t room = sizeof c->said_text - 1, k = 0;
	size_t nb = strlen(before);
	if (nb > 0) {
		k = nb < room ? nb : room;
		memcpy(c->said_text, before, k);
		if (k < room) {
			c->said_text[k++] = '\n';
		}
	}
	size_t take = n < room - k ? n : room - k;
	memcpy(c->said_text + k, line, take);
	c->said_text[k + take] = '\0';
	pthread_mutex_unlock(&c->said_lock);
	check_tell(c, CHECK_SAID);
}

static int conv(int n, const struct pam_message **msg, struct pam_response **resp, void *data)
{
	struct check *c = data;
	struct pam_response *r = calloc((size_t)n, sizeof *r);
	if (!r) {
		return PAM_BUF_ERR;
	}
	for (int i = 0; i < n; i++) {
		int style = msg[i]->msg_style;
		if (style == PAM_PROMPT_ECHO_OFF || style == PAM_PROMPT_ECHO_ON) {
			r[i].resp = strdup(c->pass);
			if (!r[i].resp) {
				for (int k = 0; k < i; k++) {
					free(r[k].resp);
				}
				free(r);
				return PAM_BUF_ERR;
			}
		} else if (style == PAM_ERROR_MSG || style == PAM_TEXT_INFO) {
			wlr_log(WLR_INFO, "lock: PAM says: %s", msg[i]->msg ? msg[i]->msg : "");
			check_said(c, msg[i]->msg);
		}
	}
	*resp = r;
	return PAM_SUCCESS;
}

static void *check_thread(void *arg)
{
	struct check *c = arg;
	struct pam_conv pc = { conv, c };
	pam_handle_t *h = NULL;
	const char *service = access("/etc/pam.d/desk", F_OK) == 0 ? "desk" : "login";
	int r = c->confdir[0] ? pam_start_confdir("desk", c->user, &pc, c->confdir, &h)
		: pam_start(service, c->user, &pc, &h);
	if (r == PAM_SUCCESS) {
		r = pam_authenticate(h, 0);
		if (r == PAM_SUCCESS) {
			/* tickets and the like, as a login refreshes them */
			pam_setcred(h, PAM_REFRESH_CRED);
		}
		pam_end(h, r);
	}
	c->ok = r == PAM_SUCCESS;
	explicit_bzero(c->pass, sizeof c->pass);
	/* the last the thread touches of it: the loop frees it on reading this */
	check_tell(c, CHECK_DONE);
	return NULL;
}

static int check_landed(int fd, uint32_t mask, void *data)
{
	(void)mask;
	struct check *c = data;
	/* everything the thread has sent so far, in the order sent: what was
	 * said comes before the end */
	bool spoke = false, over = false;
	char buf[64];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof buf);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			break;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == CHECK_SAID) {
				spoke = true;
			} else {
				over = true;
			}
		}
	}
	if (spoke && c->said) {
		char text[sizeof c->said_text];
		pthread_mutex_lock(&c->said_lock);
		memcpy(text, c->said_text, sizeof text);
		pthread_mutex_unlock(&c->said_lock);
		c->said(text, c->data);
	}
	if (!over) {
		return 0;
	}
	wl_event_source_remove(c->src);
	close(c->fd[0]);
	close(c->fd[1]);
	pthread_mutex_destroy(&c->said_lock);
	void (*done)(bool, void *) = c->done;
	void *d = c->data;
	bool ok = c->ok;
	free(c);
	done(ok, d);
	return 0;
}

bool desk_lock_check(struct wl_event_loop *loop, const char *password,
	void (*done)(bool ok, void *data), void (*said)(const char *text, void *data),
	void *data)
{
	struct check *c = calloc(1, sizeof *c);
	if (!c) {
		return false;
	}
	char name[128];
	desk_lock_user(c->user, sizeof c->user, name, sizeof name);
	snprintf(c->pass, sizeof c->pass, "%s", password);
	const char *dir = getenv("DESK_LOCK_PAM_DIR");
	if (dir && *dir && !desk_session_is_desktop()) {
		snprintf(c->confdir, sizeof c->confdir, "%s", dir);
	}
	c->done = done;
	c->said = said;
	c->data = data;
	pthread_mutex_init(&c->said_lock, NULL);
	/* not blocking: the loop reads until there is nothing more */
	if (pipe2(c->fd, O_CLOEXEC | O_NONBLOCK) != 0) {
		explicit_bzero(c->pass, sizeof c->pass);
		pthread_mutex_destroy(&c->said_lock);
		free(c);
		return false;
	}
	c->src = wl_event_loop_add_fd(loop, c->fd[0], WL_EVENT_READABLE, check_landed, c);
	pthread_t th;
	if (!c->src || pthread_create(&th, NULL, check_thread, c) != 0) {
		if (c->src) {
			wl_event_source_remove(c->src);
		}
		close(c->fd[0]);
		close(c->fd[1]);
		explicit_bzero(c->pass, sizeof c->pass);
		pthread_mutex_destroy(&c->said_lock);
		free(c);
		return false;
	}
	pthread_detach(th);
	return true;
}

/* ---- logind ---- */

#ifdef DESK_HAVE_POST
static sd_bus *g_sys;
static char g_session_path[256];
static void (*g_lock)(void *), (*g_unlock)(void *);
static void *g_data;

static int on_lock(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)m;
	(void)data;
	(void)err;
	wlr_log(WLR_INFO, "lock: logind says lock");
	g_lock(g_data);
	return 0;
}

static int on_unlock(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)m;
	(void)data;
	(void)err;
	wlr_log(WLR_INFO, "lock: logind says unlock");
	g_unlock(g_data);
	return 0;
}

/* The machine going to sleep waits for the door: logind's "delay" lock on
 * sleep, held while awake and let go once the door has been on every lit
 * screen (desk_lock_sleep_ready) -- or after SLEEP_HOLD_MS whatever
 * happens, inside logind's own limit (InhibitDelayMaxSec, five seconds
 * unless set otherwise). Without it the machine went down with the desk
 * still on the screen, and opening the lid showed it before the door. */
#define SLEEP_HOLD_MS 4000
static int g_sleep_fd = -1;
static bool g_sleep_pending;
static struct wl_event_source *g_sleep_timer;

static void sleep_let_go(void)
{
	g_sleep_pending = false;
	if (g_sleep_timer) {
		wl_event_source_timer_update(g_sleep_timer, 0);
	}
	if (g_sleep_fd >= 0) {
		close(g_sleep_fd);
		g_sleep_fd = -1;
		wlr_log(WLR_INFO, "lock: the door is down; the machine may sleep");
	}
}

static int sleep_timeout(void *data)
{
	(void)data;
	if (g_sleep_pending) {
		wlr_log(WLR_INFO, "lock: the door was not shown in time; the machine sleeps anyway");
	}
	sleep_let_go();
	return 0;
}

static int sleep_held(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)data;
	(void)err;
	int fd = -1;
	if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "h", &fd) < 0
		|| fd < 0) {
		const sd_bus_error *e = sd_bus_message_get_error(m);
		wlr_log(WLR_INFO, "lock: logind will not wait for the door (%s)",
			e && e->message ? e->message : "no answer");
		return 0;
	}
	/* the message owns its descriptor: a copy of our own, kept from the
	 * programs the desk starts */
	int mine = fcntl(fd, F_DUPFD_CLOEXEC, 3);
	if (mine < 0) {
		return 0;
	}
	if (g_sleep_fd >= 0) {
		close(g_sleep_fd);
	}
	g_sleep_fd = mine;
	return 0;
}

/* Asked for, not waited for: the answer comes through the loop. */
static void sleep_hold(void)
{
	if (g_sleep_fd >= 0) {
		return;
	}
	if (sd_bus_call_method_async(g_sys, NULL, "org.freedesktop.login1",
			"/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
			sleep_held, NULL, "ssss", "sleep", "DEsk", "Bringing the front door down first",
			"delay") < 0) {
		wlr_log(WLR_INFO, "lock: could not ask logind to wait for the door");
	}
}

static int on_sleep(sd_bus_message *m, void *data, sd_bus_error *err)
{
	(void)data;
	(void)err;
	int going = 0;
	if (sd_bus_message_read(m, "b", &going) >= 0 && going) {
		wlr_log(WLR_INFO, "lock: the machine is going to sleep");
		/* before the lock: with no screen lit it is ready at once */
		g_sleep_pending = g_sleep_fd >= 0;
		if (g_sleep_pending && g_sleep_timer) {
			wl_event_source_timer_update(g_sleep_timer, SLEEP_HOLD_MS);
		}
		g_lock(g_data);
	} else {
		/* awake: held again for the next time */
		sleep_let_go();
		sleep_hold();
	}
	return 0;
}

bool desk_lock_sleep_pending(void)
{
	return g_sleep_pending;
}

void desk_lock_sleep_ready(void)
{
	if (g_sleep_pending) {
		sleep_let_go();
	}
}

static int bus_ready(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	(void)data;
	while (sd_bus_process(g_sys, NULL) > 0) {
		;
	}
	return 0;
}

bool desk_lock_watch(struct wl_event_loop *loop, void (*lock)(void *data),
	void (*unlock)(void *data), void *data)
{
	if (!desk_session_is_desktop() || sd_bus_open_system(&g_sys) < 0) {
		return false;
	}
	sd_bus_error err = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	const char *id = getenv("XDG_SESSION_ID");
	int r = id && *id
		? sd_bus_call_method(g_sys, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "GetSession", &err, &reply, "s", id)
		: sd_bus_call_method(g_sys, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", "GetSessionByPID", &err, &reply, "u",
			(uint32_t)getpid());
	const char *path = NULL;
	if (r < 0 || sd_bus_message_read(reply, "o", &path) < 0 || !path) {
		wlr_log(WLR_INFO, "lock: no logind session to watch (%s)",
			err.message ? err.message : "no answer");
		sd_bus_error_free(&err);
		sd_bus_message_unref(reply);
		sd_bus_unref(g_sys);
		g_sys = NULL;
		return false;
	}
	snprintf(g_session_path, sizeof g_session_path, "%s", path);
	sd_bus_message_unref(reply);
	g_lock = lock;
	g_unlock = unlock;
	g_data = data;
	sd_bus_match_signal(g_sys, NULL, "org.freedesktop.login1", g_session_path,
		"org.freedesktop.login1.Session", "Lock", on_lock, NULL);
	sd_bus_match_signal(g_sys, NULL, "org.freedesktop.login1", g_session_path,
		"org.freedesktop.login1.Session", "Unlock", on_unlock, NULL);
	sd_bus_match_signal(g_sys, NULL, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "PrepareForSleep", on_sleep, NULL);
	g_sleep_timer = wl_event_loop_add_timer(loop, sleep_timeout, NULL);
	sleep_hold();
	wl_event_loop_add_fd(loop, sd_bus_get_fd(g_sys), WL_EVENT_READABLE, bus_ready, NULL);
	bus_ready(0, 0, NULL);
	wlr_log(WLR_INFO, "lock: watching %s", g_session_path);
	return true;
}

void desk_lock_hint(bool locked)
{
	if (!g_sys || !g_session_path[0]) {
		return;
	}
	sd_bus_call_method(g_sys, "org.freedesktop.login1", g_session_path,
		"org.freedesktop.login1.Session", "SetLockedHint", NULL, NULL, "b", (int)locked);
}
#else
bool desk_lock_watch(struct wl_event_loop *loop, void (*lock)(void *data),
	void (*unlock)(void *data), void *data)
{
	(void)loop;
	(void)lock;
	(void)unlock;
	(void)data;
	return false;
}

void desk_lock_hint(bool locked)
{
	(void)locked;
}

bool desk_lock_sleep_pending(void)
{
	return false;
}

void desk_lock_sleep_ready(void)
{
}
#endif
