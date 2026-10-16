/* The desktop session (session.h).
 *
 * Autostart is the freedesktop.org one: every *.desktop file in
 * $XDG_CONFIG_HOME/autostart and in autostart/ under each of
 * $XDG_CONFIG_DIRS, a file of one name in an earlier directory standing in
 * for any of that name in a later one (which is how a user turns one off:
 * a copy in ~/.config/autostart with Hidden=true). An entry runs unless it
 * is hidden, turned off (X-GNOME-Autostart-enabled=false), meant only for
 * other desktops (OnlyShowIn without DEsk, NotShowIn with it), or its
 * TryExec is not installed.
 *
 * And a few that would fight the desk are left out whatever they say,
 * because they are parts of other desktops that name no desktop: a second
 * notification daemon (the post box is DEsk's), a screen locker or a
 * screensaver for X, a colour daemon for X, the authentication agents of
 * other desktops (DEsk starts one of its own, desk-session-helper), and
 * the network applet's own entry (DEsk starts it in the form the tray can
 * show). */

/* syscall(), for pidfd_open, which glibc has had a name for only lately */
#define _DEFAULT_SOURCE
#include "session.h"
#include "apps.h"

#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

extern char **environ;

/* ---- waiting without the desk waiting ----
 *
 * The session's few programs are waited for -- what comes after one may
 * need it done -- but not by the desk standing still: a program's end comes
 * to the event loop as its pidfd turning readable, and what follows it runs
 * then. Waited for in place, they held the first frame back by however
 * long systemd and D-Bus took to answer, which on a slow machine at login
 * is a good part of a second. Where there are no pidfds (a kernel before
 * 5.3) they are waited for in place still. */

static struct wl_event_loop *g_loop;

struct child_wait {
	pid_t pid;
	int fd;
	struct wl_event_source *src;
	void (*then)(int status);
};

static int child_done(int fd, uint32_t mask, void *data)
{
	(void)fd;
	(void)mask;
	struct child_wait *w = data;
	int st = 0;
	while (waitpid(w->pid, &st, 0) < 0 && errno == EINTR) {
		;
	}
	wl_event_source_remove(w->src);
	close(w->fd);
	void (*then)(int) = w->then;
	free(w);
	if (then) {
		then(st);
	}
	return 0;
}

/* pid waited for, and then (if there is one) run with how it ended */
static void after(pid_t pid, void (*then)(int status))
{
	int fd = -1;
#ifdef SYS_pidfd_open
	if (g_loop) {
		fd = (int)syscall(SYS_pidfd_open, pid, 0);
	}
#endif
	struct child_wait *w = fd >= 0 ? calloc(1, sizeof(*w)) : NULL;
	if (w) {
		w->pid = pid;
		w->fd = fd;
		w->then = then;
		w->src = wl_event_loop_add_fd(g_loop, fd, WL_EVENT_READABLE, child_done, w);
		if (w->src) {
			return;
		}
		free(w);
	}
	if (fd >= 0) {
		close(fd);
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
		;
	}
	if (then) {
		then(st);
	}
}

bool desk_session_is_desktop(void)
{
	const char *s = getenv("DESK_SESSION");
	return s && !strcmp(s, "1");
}

/* ---- telling D-Bus and systemd where the display is ---- */

static bool on_path(const char *name, char *out, size_t n)
{
	if (strchr(name, '/')) {
		if (access(name, X_OK) == 0) {
			snprintf(out, n, "%s", name);
			return true;
		}
		return false;
	}
	const char *path = getenv("PATH");
	char dir[1024];
	for (const char *p = path && *path ? path : "/usr/local/bin:/usr/bin:/bin"; *p;) {
		const char *e = strchr(p, ':');
		size_t k = e ? (size_t)(e - p) : strlen(p);
		if (k > 0 && k < sizeof dir) {
			memcpy(dir, p, k);
			dir[k] = '\0';
			if ((size_t)snprintf(out, n, "%s/%s", dir, name) < n && access(out, X_OK) == 0) {
				return true;
			}
		}
		if (!e) {
			break;
		}
		p = e + 1;
	}
	return false;
}

/* The variables a program started for the session needs to find it. Only
 * those that are set are passed on. All of them are taken back when the
 * session is over (desk-session-helper end, which names them too): another
 * desktop is not to get them. */
static const char *const ACTIVATION_VARS[] = {
	"WAYLAND_DISPLAY", "DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
	"XDG_SESSION_DESKTOP", "DESKTOP_SESSION", "QT_QPA_PLATFORM", "QT_QPA_PLATFORMTHEME",
	"GDK_BACKEND", "MOZ_ENABLE_WAYLAND", "GTK_IM_MODULE", "QT_IM_MODULE", "QT_IM_MODULES",
	"XMODIFIERS", "DESK_IM", "XCURSOR_THEME", "XCURSOR_SIZE",
	/* DEsk's own layer of settings over the user's (desk-session): what
	 * D-Bus starts for the session -- a file manager asked to show a
	 * folder -- looks as the rest of the session does. */
	"DCONF_PROFILE",
	/* Qt's programs as big as the desk's things, on a dense screen
	 * (wm.c, scale_programs) */
	"QT_SCALE_FACTOR",
};

/* What a program must not be told when it is not set here: with IBus,
 * programs type through DEsk, and an earlier session (or another
 * desktop) may have left GTK_IM_MODULE=ibus in systemd's environment, which
 * would send whatever D-Bus starts straight to IBus, round DEsk.
 * dbus-update-activation-environment can only set; systemd can unset. */
static const char *const UNSET_VARS[] = { "GTK_IM_MODULE", "QT_IM_MODULE" };

static void unset_activation_environment(void)
{
	char prog[1024];
	if (!on_path("systemctl", prog, sizeof prog)) {
		return;
	}
	char *argv[8];
	int n = 0;
	argv[n++] = prog;
	argv[n++] = "--user";
	argv[n++] = "unset-environment";
	for (size_t i = 0; i < sizeof UNSET_VARS / sizeof UNSET_VARS[0]; i++) {
		if (!getenv(UNSET_VARS[i])) {
			argv[n++] = (char *)UNSET_VARS[i];
		}
	}
	if (n == 3) {
		return;
	}
	argv[n] = NULL;
	pid_t pid;
	if (posix_spawn(&pid, prog, NULL, NULL, argv, environ) != 0) {
		return;
	}
	after(pid, NULL);
}

/* told, or not: then the programs that start with the session */
static void autostart(void);

static void environment_told(int st)
{
	wlr_log(WLR_INFO, "session: D-Bus and systemd told where the display is%s",
		WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "" : " (it said no)");
	unset_activation_environment();
	autostart();
}

/* -1 when there is nothing to wait for */
static pid_t update_activation_environment(void)
{
	char prog[1024];
	if (!on_path("dbus-update-activation-environment", prog, sizeof prog)) {
		wlr_log(WLR_INFO, "session: no dbus-update-activation-environment; what D-Bus "
			"starts will not know where the display is");
		return -1;
	}
	char *argv[4 + sizeof ACTIVATION_VARS / sizeof ACTIVATION_VARS[0]];
	int n = 0;
	argv[n++] = prog;
	argv[n++] = "--systemd";
	for (size_t i = 0; i < sizeof ACTIVATION_VARS / sizeof ACTIVATION_VARS[0]; i++) {
		if (getenv(ACTIVATION_VARS[i])) {
			argv[n++] = (char *)ACTIVATION_VARS[i];
		}
	}
	argv[n] = NULL;
	pid_t pid;
	if (posix_spawn(&pid, prog, NULL, NULL, argv, environ) != 0) {
		return -1;
	}
	return pid;
}

/* ---- autostart ---- */

/* parts of other desktops that name no desktop, and would fight this one */
static const char *const LEFT_OUT[] = {
	"light-locker.desktop", "xscreensaver.desktop", "xiccd.desktop",
	"dunst.desktop", "mako.desktop", "xfce4-notifyd.desktop", "notification-daemon.desktop",
	"polkit-mate-authentication-agent-1.desktop", "polkit-gnome-authentication-agent-1.desktop",
	"polkit-kde-authentication-agent-1.desktop", "lxpolkit.desktop",
	"lxqt-policykit-agent.desktop", "nm-applet.desktop", "xembedsniproxy.desktop",
};

static bool left_out(const char *file)
{
	for (size_t i = 0; i < sizeof LEFT_OUT / sizeof LEFT_OUT[0]; i++) {
		if (!strcmp(file, LEFT_OUT[i])) {
			return true;
		}
	}
	return false;
}

/* Is DEsk in a list like "KDE;GNOME;"? It goes by DEsk there (and by
 * DeskWM, the name it had before, in what was written for that). */
static bool lists_us(const char *list)
{
	const char *p = list;
	while (*p) {
		const char *e = strchr(p, ';');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if ((n == 4 && !strncmp(p, "DEsk", 4)) || (n == 6 && !strncmp(p, "DeskWM", 6))) {
			return true;
		}
		if (!e) {
			break;
		}
		p = e + 1;
	}
	return false;
}

struct entry {
	char exec[1024], try_exec[512], only[256], not_in[256], name[128], path[512];
	bool hidden, off, application;
};

static bool read_entry(const char *file, struct entry *e)
{
	FILE *f = fopen(file, "re");
	if (!f) {
		return false;
	}
	memset(e, 0, sizeof *e);
	e->application = true;
	char line[2048];
	bool in = false;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			in = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!in || line[0] == '#') {
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *k = line, *v = eq + 1;
		/* keys have no spaces round them in practice, but the spec allows */
		while (*v == ' ') {
			v++;
		}
		char key[64];
		size_t kl = strlen(k);
		while (kl > 0 && k[kl - 1] == ' ') {
			kl--;
		}
		if (kl >= sizeof key) {
			continue;
		}
		memcpy(key, k, kl);
		key[kl] = '\0';
		if (!strcmp(key, "Exec")) {
			snprintf(e->exec, sizeof e->exec, "%s", v);
		} else if (!strcmp(key, "TryExec")) {
			snprintf(e->try_exec, sizeof e->try_exec, "%s", v);
		} else if (!strcmp(key, "OnlyShowIn")) {
			snprintf(e->only, sizeof e->only, "%s", v);
		} else if (!strcmp(key, "NotShowIn")) {
			snprintf(e->not_in, sizeof e->not_in, "%s", v);
		} else if (!strcmp(key, "Name")) {
			snprintf(e->name, sizeof e->name, "%s", v);
		} else if (!strcmp(key, "Path")) {
			snprintf(e->path, sizeof e->path, "%s", v);
		} else if (!strcmp(key, "Hidden")) {
			e->hidden = !strcmp(v, "true");
		} else if (!strcmp(key, "X-GNOME-Autostart-enabled")) {
			e->off = !strcmp(v, "false");
		} else if (!strcmp(key, "Type")) {
			e->application = !strcmp(v, "Application");
		}
	}
	fclose(f);
	return true;
}

/* names already decided on, from a directory that comes first */
#define AUTOSTART_MAX 256
static char g_seen[AUTOSTART_MAX][128];
static int g_nseen;

static bool seen(const char *file)
{
	for (int i = 0; i < g_nseen; i++) {
		if (!strcmp(g_seen[i], file)) {
			return true;
		}
	}
	if (g_nseen < AUTOSTART_MAX) {
		snprintf(g_seen[g_nseen++], sizeof g_seen[0], "%s", file);
	}
	return false;
}

static void autostart_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);
		if (len < 9 || strcmp(de->d_name + len - 8, ".desktop") || seen(de->d_name)) {
			continue;
		}
		char file[1024];
		snprintf(file, sizeof file, "%s/%s", dir, de->d_name);
		struct entry e;
		if (!read_entry(file, &e) || !e.application || !e.exec[0] || e.hidden || e.off) {
			continue;
		}
		if ((e.only[0] && !lists_us(e.only)) || (e.not_in[0] && lists_us(e.not_in))) {
			continue;
		}
		if (left_out(de->d_name)) {
			wlr_log(WLR_INFO, "autostart: not %s (left out under DEsk)", de->d_name);
			continue;
		}
		char found[1024];
		if (e.try_exec[0] && !on_path(e.try_exec, found, sizeof found)) {
			continue;
		}
		char pool[2048];
		char *argv[DESK_APP_ARGV_MAX];
		if (desk_exec_argv(e.exec, argv, DESK_APP_ARGV_MAX, pool, sizeof pool) < 0
			|| !on_path(argv[0], found, sizeof found)) {
			continue;
		}
		if (desk_exec_launch(argv, e.path) > 0) {
			wlr_log(WLR_INFO, "autostart: %s (%s)", e.name[0] ? e.name : de->d_name,
				de->d_name);
		}
	}
	closedir(d);
}

static void autostart(void)
{
	char dir[2100];
	const char *ch = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	if (ch && *ch) {
		snprintf(dir, sizeof dir, "%s/autostart", ch);
		autostart_dir(dir);
	} else if (home && *home) {
		snprintf(dir, sizeof dir, "%s/.config/autostart", home);
		autostart_dir(dir);
	}
	const char *cd = getenv("XDG_CONFIG_DIRS");
	char dirs[2048];
	snprintf(dirs, sizeof dirs, "%s", cd && *cd ? cd : "/etc/xdg");
	for (char *p = dirs; p && *p;) {
		char *e = strchr(p, ':');
		if (e) {
			*e = '\0';
		}
		if ((size_t)snprintf(dir, sizeof dir, "%s/autostart", p) < sizeof dir) {
			autostart_dir(dir);
		}
		p = e ? e + 1 : NULL;
	}
#ifdef DESK_DATADIR
	/* and DEsk's own, last: a file of the same name anywhere above
	 * stands in for one of these */
	autostart_dir(DESK_DATADIR "/desk/autostart");
#endif
}

void desk_session_start(struct wl_event_loop *loop)
{
	if (!desk_session_is_desktop()) {
		return;
	}
	g_loop = loop;
	/* waited for (environment_told): what autostart starts next may ask
	 * D-Bus for something at once, and it has to find the display there */
	pid_t pid = update_activation_environment();
	if (pid > 0) {
		after(pid, environment_told);
	} else {
		unset_activation_environment();
		autostart();
	}
}
