// SPDX-License-Identifier: GPL-2.0-only
#define _POSIX_C_SOURCE 200809L
#include <getopt.h>
#include <pango/pangocairo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common/fd-util.h"
#include "common/font.h"
#include "common/spawn.h"
#include "config/keybind.h"
#include "config/rcxml.h"
#include "config/session.h"
#include "labwc.h"
#include "theme.h"
#include "translate.h"
#include "menu/menu.h"

struct rcxml rc = { 0 };

static const struct option long_options[] = {
	{"config", required_argument, NULL, 'c'},
	{"config-dir", required_argument, NULL, 'C'},
	{"debug", no_argument, NULL, 'd'},
	{"exit", no_argument, NULL, 'e'},
	{"help", no_argument, NULL, 'h'},
	{"merge-config", no_argument, NULL, 'm'},
	{"reconfigure", no_argument, NULL, 'r'},
	{"startup", required_argument, NULL, 's'},
	{"session", required_argument, NULL, 'S'},
	{"version", no_argument, NULL, 'v'},
	{"verbose", no_argument, NULL, 'V'},
	{"enable-keybind", required_argument, NULL, 1000},
	{"disable-keybind", required_argument, NULL, 1001},
	{"toggle-keybind", required_argument, NULL, 1002},
	{"workspace-switch", required_argument, NULL, 2000},
	{"workspace-next", no_argument, NULL, 2001},
	{"workspace-prev", no_argument, NULL, 2002},
	{"workspace-current", no_argument, NULL, 2003},
	{"enable-tiling", no_argument, NULL, 3000},
	{"disable-tiling", no_argument, NULL, 3001},
	{"toggle-tiling", no_argument, NULL, 3002},
	{"tiling-grid-mode", required_argument, NULL, 3003},
	{"recalculate-tiling", no_argument, NULL, 3004},
	{"tiling-status", no_argument, NULL, 3005},
	{0, 0, 0, 0}
};

static const char labwc_usage[] =
"Usage: labwc [options...]\n"
"  -c, --config <file>      Specify config file (with path)\n"
"  -C, --config-dir <dir>   Specify config directory\n"
"  -d, --debug              Enable full logging, including debug information\n"
"  -e, --exit               Exit the compositor\n"
"  -h, --help               Show help message and quit\n"
"  -m, --merge-config       Merge user config files/theme in all XDG Base Dirs\n"
"  -r, --reconfigure        Reload the compositor configuration\n"
"  -s, --startup <command>  Run command on startup\n"
"  -S, --session <command>  Run command on startup and terminate on exit\n"
"  -v, --version            Show version number and quit\n"
"  -V, --verbose            Enable more verbose logging\n"
"      --enable-keybind <id>   Enable a toggleable keybind\n"
"      --disable-keybind <id>  Disable a toggleable keybind\n"
"      --toggle-keybind <id>   Toggle a toggleable keybind\n"
"      --workspace-switch <number|name>  Switch to a workspace by number or name\n"
"      --workspace-next          Switch to next workspace\n"
"      --workspace-prev          Switch to previous workspace\n"
"      --workspace-current       Query the active workspace\n"
"      --enable-tiling           Enable automatic tiling mode\n"
"      --disable-tiling          Disable automatic tiling mode\n"
"      --toggle-tiling           Toggle automatic tiling mode on/off\n"
"      --tiling-grid-mode <on|off|toggle>  Set grid snapping mode (on=simple grid, off=smart resize preservation)\n"
"      --recalculate-tiling      Recalculate and rearrange tiled windows\n"
"      --tiling-status           Query the current tiling mode (stacking/grid/smart)\n";

static void
usage(void)
{
	printf("%s", labwc_usage);
	exit(0);
}

static void
print_version(void)
{
	#define FEATURE_ENABLED(feature) (HAVE_##feature ? "+" : "-")
	printf("labwc %s (%sxwayland %snls %srsvg %slibsfdo)\n",
		LABWC_VERSION,
		FEATURE_ENABLED(XWAYLAND),
		FEATURE_ENABLED(NLS),
		FEATURE_ENABLED(RSVG),
		FEATURE_ENABLED(LIBSFDO)
	);
	#undef FEATURE_ENABLED
}

static void
die_on_detecting_suid(void)
{
	if (geteuid() != 0 && getegid() != 0) {
		return;
	}
	if (getuid() == geteuid() && getgid() == getegid()) {
		return;
	}
	wlr_log(WLR_ERROR, "SUID detected - aborting");
	exit(EXIT_FAILURE);
}

static void
die_on_no_fonts(void)
{
	PangoContext *context = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	PangoLayout *layout = pango_layout_new(context);
	pango_layout_set_text(layout, "abcdefg", -1);
	int nr_unknown_glyphs = pango_layout_get_unknown_glyphs_count(layout);
	g_object_unref(layout);
	g_object_unref(context);

	if (nr_unknown_glyphs > 0) {
		wlr_log(WLR_ERROR, "no fonts are available");
		exit(EXIT_FAILURE);
	}

	/*
	 * Make pango's dedicated thread exit. This prevents CI failures due to
	 * SIGTERM delivered to the pango's thread. This kind of workaround is
	 * not needed after we register our SIGTERM handler in
	 * server_init() > wl_event_loop_add_signal(), which masks SIGTERM.
	 */
	pango_cairo_font_map_set_default(NULL);
}

static void
send_signal_to_labwc_pid(int signal)
{
	char *labwc_pid = getenv("LABWC_PID");
	if (!labwc_pid) {
		wlr_log(WLR_ERROR, "LABWC_PID not set");
		exit(EXIT_FAILURE);
	}
	int pid = atoi(labwc_pid);
	if (!pid) {
		wlr_log(WLR_ERROR, "should not send signal to pid 0");
		exit(EXIT_FAILURE);
	}
	kill(pid, signal);
}

static void
send_keybind_command(const char *command, const char *id)
{
	char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
		exit(EXIT_FAILURE);
	}

	char *labwc_pid = getenv("LABWC_PID");
	if (!labwc_pid) {
		fprintf(stderr, "LABWC_PID not set - labwc is not running\n");
		exit(EXIT_FAILURE);
	}

	char cmd_file[256];
	snprintf(cmd_file, sizeof(cmd_file), "%s/labwc-keybind-cmd", runtime_dir);

	FILE *f = fopen(cmd_file, "w");
	if (!f) {
		perror("Failed to open command file");
		exit(EXIT_FAILURE);
	}

	fprintf(f, "%s %s\n", command, id);
	fclose(f);

	/* Trigger the running instance to process the command */
	send_signal_to_labwc_pid(SIGUSR1);
}

static void
send_tiling_command(const char *command, const char *arg)
{
	char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
		exit(EXIT_FAILURE);
	}

	char *labwc_pid = getenv("LABWC_PID");
	if (!labwc_pid) {
		fprintf(stderr, "LABWC_PID not set - labwc is not running\n");
		exit(EXIT_FAILURE);
	}

	char cmd_file[256];
	snprintf(cmd_file, sizeof(cmd_file), "%s/labwc-tiling-cmd", runtime_dir);

	FILE *f = fopen(cmd_file, "w");
	if (!f) {
		perror("Failed to open command file");
		exit(EXIT_FAILURE);
	}

	if (arg) {
		fprintf(f, "%s %s\n", command, arg);
	} else {
		fprintf(f, "%s\n", command);
	}
	fclose(f);

	/* Trigger the running instance to process the command */
	send_signal_to_labwc_pid(SIGUSR1);
}

static void
send_workspace_command(const char *command, const char *arg)
{
	char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
		exit(EXIT_FAILURE);
	}

	char *labwc_pid = getenv("LABWC_PID");
	if (!labwc_pid) {
		fprintf(stderr, "LABWC_PID not set - labwc is not running\n");
		exit(EXIT_FAILURE);
	}

	char cmd_file[256];
	snprintf(cmd_file, sizeof(cmd_file), "%s/labwc-workspace-cmd", runtime_dir);

	FILE *f = fopen(cmd_file, "w");
	if (!f) {
		perror("Failed to open command file");
		exit(EXIT_FAILURE);
	}

	if (arg) {
		fprintf(f, "%s %s\n", command, arg);
	} else {
		fprintf(f, "%s\n", command);
	}
	fclose(f);

	/* Trigger the running instance to process the command */
	send_signal_to_labwc_pid(SIGUSR1);
}

static void
query_workspace_current(void)
{
	char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
		exit(EXIT_FAILURE);
	}

	char status_file[256];
	snprintf(status_file, sizeof(status_file), "%s/labwc-workspace-current", runtime_dir);

	FILE *f = fopen(status_file, "r");
	if (!f) {
		fprintf(stderr, "Failed to read workspace status file\n");
		exit(EXIT_FAILURE);
	}

	char workspace[256];
	if (fgets(workspace, sizeof(workspace), f)) {
		/* Remove trailing newline if present */
		size_t len = strlen(workspace);
		if (len > 0 && workspace[len - 1] == '\n') {
			workspace[len - 1] = '\0';
		}
		printf("%s\n", workspace);
	} else {
		fprintf(stderr, "Failed to read workspace name\n");
		fclose(f);
		exit(EXIT_FAILURE);
	}

	fclose(f);
	exit(0);
}

static void
query_tiling_status(void)
{
	char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr, "XDG_RUNTIME_DIR not set\n");
		exit(EXIT_FAILURE);
	}

	char status_file[256];
	snprintf(status_file, sizeof(status_file), "%s/labwc-tiling-status", runtime_dir);

	FILE *f = fopen(status_file, "r");
	if (!f) {
		fprintf(stderr, "Failed to read tiling status file\n");
		exit(EXIT_FAILURE);
	}

	char status[256];
	if (fgets(status, sizeof(status), f)) {
		/* Remove trailing newline if present */
		size_t len = strlen(status);
		if (len > 0 && status[len - 1] == '\n') {
			status[len - 1] = '\0';
		}
		printf("%s\n", status);
	} else {
		fprintf(stderr, "Failed to read tiling status\n");
		fclose(f);
		exit(EXIT_FAILURE);
	}

	fclose(f);
	exit(0);
}

struct idle_ctx {
	struct server *server;
	const char *primary_client;
	const char *startup_cmd;
};

static void
idle_callback(void *data)
{
	/* Idle callbacks destroy automatically once triggered */
	struct idle_ctx *ctx = data;

	/* Start session-manager if one is specified by -S|--session */
	if (ctx->primary_client) {
		ctx->server->primary_client_pid = spawn_primary_client(ctx->primary_client);
		if (ctx->server->primary_client_pid < 0) {
			wlr_log(WLR_ERROR, "fatal error starting primary client: %s",
				ctx->primary_client);
			wl_display_terminate(ctx->server->wl_display);
			return;
		}
	}

	session_autostart_init(ctx->server);
	if (ctx->startup_cmd) {
		spawn_async_no_shell(ctx->startup_cmd);
	}
}

int
main(int argc, char *argv[])
{
	char *startup_cmd = NULL;
	char *primary_client = NULL;
	enum wlr_log_importance verbosity = WLR_ERROR;

	int c;
	while (1) {
		int index = 0;
		c = getopt_long(argc, argv, "c:C:dehmrs:S:vV", long_options, &index);
		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':
			rc.config_file = optarg;
			break;
		case 'C':
			rc.config_dir = optarg;
			break;
		case 'd':
			verbosity = WLR_DEBUG;
			break;
		case 'e':
			send_signal_to_labwc_pid(SIGTERM);
			exit(0);
		case 'm':
			rc.merge_config = true;
			break;
		case 'r':
			send_signal_to_labwc_pid(SIGHUP);
			exit(0);
		case 's':
			startup_cmd = optarg;
			break;
		case 'S':
			primary_client = optarg;
			break;
		case 'v':
			print_version();
			exit(0);
		case 'V':
			verbosity = WLR_INFO;
			break;
		case 1000: /* --enable-keybind */
			send_keybind_command("enable", optarg);
			exit(0);
		case 1001: /* --disable-keybind */
			send_keybind_command("disable", optarg);
			exit(0);
		case 1002: /* --toggle-keybind */
			send_keybind_command("toggle", optarg);
			exit(0);
		case 2000: /* --workspace-switch */
			send_workspace_command("switch", optarg);
			exit(0);
		case 2001: /* --workspace-next */
			send_workspace_command("next", NULL);
			exit(0);
		case 2002: /* --workspace-prev */
			send_workspace_command("prev", NULL);
			exit(0);
		case 2003: /* --workspace-current */
			query_workspace_current();
			break;
		case 3000: /* --enable-tiling */
			send_tiling_command("enable", NULL);
			exit(0);
		case 3001: /* --disable-tiling */
			send_tiling_command("disable", NULL);
			exit(0);
		case 3002: /* --toggle-tiling */
			send_tiling_command("toggle", NULL);
			exit(0);
		case 3003: /* --tiling-grid-mode */
			send_tiling_command("grid-mode", optarg);
			exit(0);
		case 3004: /* --recalculate-tiling */
			send_tiling_command("recalculate", NULL);
			exit(0);
		case 3005: /* --tiling-status */
			query_tiling_status();
			break;
		case 'h':
		default:
			usage();
		}
	}
	if (optind < argc) {
		usage();
	}

	wlr_log_init(verbosity, NULL);

	die_on_detecting_suid();
	die_on_no_fonts();

	session_environment_init();

#if HAVE_NLS
	/* Initialize locale after setting env vars */
	setlocale(LC_ALL, "");
	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
	textdomain(GETTEXT_PACKAGE);
#endif

	rcxml_read(rc.config_file);

	/*
	 * Set environment variable LABWC_PID to the pid of the compositor
	 * so that SIGHUP and SIGTERM can be sent to specific instances using
	 * `kill -s <signal> <pid>` rather than `killall -s <signal> labwc`
	 */
	char pid[32];
	snprintf(pid, sizeof(pid), "%d", getpid());
	if (setenv("LABWC_PID", pid, true) < 0) {
		wlr_log_errno(WLR_ERROR, "unable to set LABWC_PID");
	} else {
		wlr_log(WLR_DEBUG, "LABWC_PID=%s", pid);
	}

	/* useful for helper programs */
	if (setenv("LABWC_VER", LABWC_VERSION, true) < 0) {
		wlr_log_errno(WLR_ERROR, "unable to set LABWC_VER");
	} else {
		wlr_log(WLR_DEBUG, "LABWC_VER=%s", LABWC_VERSION);
	}

	if (!getenv("XDG_RUNTIME_DIR")) {
		wlr_log(WLR_ERROR, "XDG_RUNTIME_DIR is unset");
		exit(EXIT_FAILURE);
	}

	increase_nofile_limit();

	struct server server = { 0 };
	server_init(&server);
	server_start(&server);

	struct theme theme = { 0 };
	theme_init(&theme, &server, rc.theme_name);
	rc.theme = &theme;
	server.theme = &theme;

	menu_init(&server);

	/* Delay startup of applications until the event loop is ready */
	struct idle_ctx idle_ctx = {
		.server = &server,
		.primary_client = primary_client,
		.startup_cmd = startup_cmd
	};
	wl_event_loop_add_idle(server.wl_event_loop, idle_callback, &idle_ctx);

	wl_display_run(server.wl_display);

	session_shutdown(&server);

	menu_finish(&server);
	theme_finish(&theme);
	rcxml_finish();
	font_finish();

	server_finish(&server);

	return 0;
}
