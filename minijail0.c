/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/types.h>
#include <unistd.h>

#include "libminijail.h"
#include "libsyscalls.h"

#include "elfparse.h"
#include "system.h"
#include "util.h"

#define IDMAP_LEN 32U

static void set_user(struct minijail *j, const char *arg, uid_t *out_uid,
		     gid_t *out_gid)
{
	char *end = NULL;
	int uid = strtod(arg, &end);
	if (!*end && *arg) {
		*out_uid = uid;
		minijail_change_uid(j, uid);
		return;
	}

	if (lookup_user(arg, out_uid, out_gid)) {
		fprintf(stderr, "Bad user: '%s'\n", arg);
		exit(1);
	}

	if (minijail_change_user(j, arg)) {
		fprintf(stderr, "Bad user: '%s'\n", arg);
		exit(1);
	}
}

static void set_group(struct minijail *j, const char *arg, gid_t *out_gid)
{
	char *end = NULL;
	int gid = strtod(arg, &end);
	if (!*end && *arg) {
		*out_gid = gid;
		minijail_change_gid(j, gid);
		return;
	}

	if (lookup_group(arg, out_gid)) {
		fprintf(stderr, "Bad group: '%s'\n", arg);
		exit(1);
	}

	if (minijail_change_group(j, arg)) {
		fprintf(stderr, "Bad group: '%s'\n", arg);
		exit(1);
	}
}

static void skip_securebits(struct minijail *j, const char *arg)
{
	uint64_t securebits_skip_mask;
	char *end = NULL;
	securebits_skip_mask = strtoull(arg, &end, 16);
	if (*end) {
		fprintf(stderr, "Invalid securebit mask: '%s'\n", arg);
		exit(1);
	}
	minijail_skip_setting_securebits(j, securebits_skip_mask);
}

static void use_caps(struct minijail *j, const char *arg)
{
	uint64_t caps;
	char *end = NULL;
	caps = strtoull(arg, &end, 16);
	if (*end) {
		fprintf(stderr, "Invalid cap set: '%s'\n", arg);
		exit(1);
	}
	minijail_use_caps(j, caps);
}

static void add_binding(struct minijail *j, char *arg)
{
	char *src = strtok(arg, ",");
	char *dest = strtok(NULL, ",");
	char *flags = strtok(NULL, ",");
	if (!src || !dest) {
		fprintf(stderr, "Bad binding: %s %s\n", src, dest);
		exit(1);
	}
	if (minijail_bind(j, src, dest, flags ? atoi(flags) : 0)) {
		fprintf(stderr, "minijail_bind failed.\n");
		exit(1);
	}
}

static void add_rlimit(struct minijail *j, char *arg)
{
	char *type = strtok(arg, ",");
	char *cur = strtok(NULL, ",");
	char *max = strtok(NULL, ",");
	if (!type || !cur || !max) {
		fprintf(stderr, "Bad rlimit '%s'.\n", arg);
		exit(1);
	}
	if (minijail_rlimit(j, atoi(type), atoi(cur), atoi(max))) {
		fprintf(stderr, "minijail_rlimit '%s,%s,%s' failed.\n", type,
			cur, max);
		exit(1);
	}
}

static void add_mount(struct minijail *j, char *arg)
{
	char *src = strtok(arg, ",");
	char *dest = strtok(NULL, ",");
	char *type = strtok(NULL, ",");
	char *flags = strtok(NULL, ",");
	char *data = strtok(NULL, ",");
	if (!src || !dest || !type) {
		fprintf(stderr, "Bad mount: %s %s %s\n", src, dest, type);
		exit(1);
	}
	if (minijail_mount_with_data(j, src, dest, type,
				     flags ? strtoul(flags, NULL, 16) : 0,
				     data)) {
		fprintf(stderr, "minijail_mount failed.\n");
		exit(1);
	}
}

static char *build_idmap(id_t id, id_t lowerid)
{
	int ret;
	char *idmap = malloc(IDMAP_LEN);
	ret = snprintf(idmap, IDMAP_LEN, "%d %d 1", id, lowerid);
	if (ret < 0 || (size_t)ret >= IDMAP_LEN) {
		free(idmap);
		fprintf(stderr, "Could not build id map.\n");
		exit(1);
	}
	return idmap;
}

static int has_cap_setgid()
{
	cap_t caps;
	cap_flag_value_t cap_value;

	if (!CAP_IS_SUPPORTED(CAP_SETGID))
		return 0;

	caps = cap_get_proc();
	if (!caps) {
		fprintf(stderr, "Could not get process' capabilities: %m\n");
		exit(1);
	}

	if (cap_get_flag(caps, CAP_SETGID, CAP_EFFECTIVE, &cap_value)) {
		fprintf(stderr, "Could not get the value of CAP_SETGID: %m\n");
		exit(1);
	}

	if (cap_free(caps)) {
		fprintf(stderr, "Could not free capabilities: %m\n");
		exit(1);
	}

	return cap_value == CAP_SET;
}

static void set_ugid_mapping(struct minijail *j, int set_uidmap, uid_t uid,
			     char *uidmap, int set_gidmap, gid_t gid,
			     char *gidmap)
{
	if (set_uidmap) {
		minijail_namespace_user(j);
		minijail_namespace_pids(j);

		if (!uidmap) {
			/*
			 * If no map is passed, map the current uid to the
			 * chosen uid in the target namespace (or root, if none
			 * was chosen).
			 */
			uidmap = build_idmap(uid, getuid());
		}
		if (0 != minijail_uidmap(j, uidmap)) {
			fprintf(stderr, "Could not set uid map.\n");
			exit(1);
		}
		free(uidmap);
	}
	if (set_gidmap) {
		minijail_namespace_user(j);
		minijail_namespace_pids(j);

		if (!gidmap) {
			/*
			 * If no map is passed, map the current gid to the
			 * chosen gid in the target namespace.
			 */
			gidmap = build_idmap(gid, getgid());
		}
		if (!has_cap_setgid()) {
			/*
			 * This means that we are not running as root,
			 * so we also have to disable setgroups(2) to
			 * be able to set the gid map.
			 * See
			 * http://man7.org/linux/man-pages/man7/user_namespaces.7.html
			 */
			minijail_namespace_user_disable_setgroups(j);
		}
		if (0 != minijail_gidmap(j, gidmap)) {
			fprintf(stderr, "Could not set gid map.\n");
			exit(1);
		}
		free(gidmap);
	}
}

static void usage(const char *progn)
{
	size_t i;
	/* clang-format off */
	printf("Usage: %s [-dGhHiIKlLnNprRstUvyYz]\n"
	       "  [-a <table>]\n"
	       "  [-b <src>,<dest>[,<writeable>]] [-k <src>,<dest>,<type>[,<flags>][,<data>]]\n"
	       "  [-c <caps>] [-C <dir>] [-P <dir>] [-e[file]] [-f <file>] [-g <group>]\n"
	       "  [-m[<uid> <loweruid> <count>]*] [-M[<gid> <lowergid> <count>]*]\n"
	       "  [-R <type,cur,max>] [-S <file>] [-t[size]] [-T <type>] [-u <user>] [-V <file>]\n"
	       "  <program> [args...]\n"
	       "  -a <table>:   Use alternate syscall table <table>.\n"
	       "  -b <...>:     Bind <src> to <dest> in chroot.\n"
	       "                Multiple instances allowed.\n"
	       "  -B <mask>:    Skip setting securebits in <mask> when restricting capabilities (-c).\n"
	       "                By default, SECURE_NOROOT, SECURE_NO_SETUID_FIXUP, and \n"
	       "                SECURE_KEEP_CAPS (together with their respective locks) are set.\n"
	       "  -k <...>:     Mount <src> at <dest> in chroot.\n"
	       "                <flags> and <data> can be specified as in mount(2).\n"
	       "                Multiple instances allowed.\n"
	       "  -c <caps>:    Restrict caps to <caps>.\n"
	       "  -C <dir>:     chroot(2) to <dir>.\n"
	       "                Not compatible with -P.\n"
	       "  -P <dir>:     pivot_root(2) to <dir> (implies -v).\n"
	       "                Not compatible with -C.\n"
	       "  --mount-dev,  Create a new /dev with a minimal set of device nodes (implies -v).\n"
	       "           -d:  See the minijail0(1) man page for the exact set.\n"
	       "  -e[file]:     Enter new network namespace, or existing one if |file| is provided.\n"
	       "  -f <file>:    Write the pid of the jailed process to <file>.\n"
	       "  -g <group>:   Change gid to <group>.\n"
	       "  -G:           Inherit supplementary groups from uid.\n"
	       "                Not compatible with -y.\n"
	       "  -y:           Keep uid's supplementary groups.\n"
	       "                Not compatible with -G.\n"
	       "  -h:           Help (this message).\n"
	       "  -H:           Seccomp filter help message.\n"
	       "  -i:           Exit immediately after fork (do not act as init).\n"
	       "  -I:           Run <program> as init (pid 1) inside a new pid namespace (implies -p).\n"
	       "  -K:           Don't mark all existing mounts as MS_PRIVATE.\n"
	       "  -l:           Enter new IPC namespace.\n"
	       "  -L:           Report blocked syscalls to syslog when using seccomp filter.\n"
	       "                Forces the following syscalls to be allowed:\n"
	       "                  ", progn);
	/* clang-format on */
	for (i = 0; i < log_syscalls_len; i++)
		printf("%s ", log_syscalls[i]);

	/* clang-format off */
	printf("\n"
	       "  -m[map]:      Set the uid map of a user namespace (implies -pU).\n"
	       "                Same arguments as newuidmap(1), multiple mappings should be separated by ',' (comma).\n"
	       "                With no mapping, map the current uid to root inside the user namespace.\n"
	       "                Not compatible with -b without the 'writable' option.\n"
	       "  -M[map]:      Set the gid map of a user namespace (implies -pU).\n"
	       "                Same arguments as newgidmap(1), multiple mappings should be separated by ',' (comma).\n"
	       "                With no mapping, map the current gid to root inside the user namespace.\n"
	       "                Not compatible with -b without the 'writable' option.\n"
	       "  -n:           Set no_new_privs.\n"
	       "  -N:           Enter a new cgroup namespace.\n"
	       "  -p:           Enter new pid namespace (implies -vr).\n"
	       "  -r:           Remount /proc read-only (implies -v).\n"
	       "  -R:           Set rlimits, can be specified multiple times.\n"
	       "  -s:           Use seccomp mode 1 (not the same as -S).\n"
	       "  -S <file>:    Set seccomp filter using <file>.\n"
	       "                E.g., '-S /usr/share/filters/<prog>.$(uname -m)'.\n"
	       "                Requires -n when not running as root.\n"
	       "  -t[size]:     Mount tmpfs at /tmp (implies -v).\n"
	       "                Optional argument specifies size (default \"64M\").\n"
	       "  -T <type>:    Assume <program> is a <type> ELF binary; <type> can be 'static' or 'dynamic'.\n"
	       "                This will avoid accessing <program> binary before execve(2).\n"
	       "                Type 'static' will avoid preload hooking.\n"
	       "  -u <user>:    Change uid to <user>.\n"
	       "  -U:           Enter new user namespace (implies -p).\n"
	       "  -v:           Enter new mount namespace.\n"
	       "  -V <file>:    Enter specified mount namespace.\n"
	       "  -w:           Create and join a new anonymous session keyring.\n"
	       "  -Y:           Synchronize seccomp filters across thread group.\n"
	       "  -z:           Don't forward signals to jailed process.\n"
	       "  --ambient:    Raise ambient capabilities. Requires -c.\n"
	       "  --uts[=name]: Enter a new UTS namespace (and set hostname).\n"
	       "  --logging=<s>:Use <s> as the logging system.\n"
	       "                <s> must be 'syslog' (default) or 'stderr'.\n");
	/* clang-format on */
}

static void seccomp_filter_usage(const char *progn)
{
	const struct syscall_entry *entry = syscall_table;
	printf("Usage: %s -S <policy.file> <program> [args...]\n\n"
	       "System call names supported:\n",
	       progn);
	for (; entry->name && entry->nr >= 0; ++entry)
		printf("  %s [%d]\n", entry->name, entry->nr);
	printf("\nSee minijail0(5) for example policies.\n");
}

static int parse_args(struct minijail *j, int argc, char *argv[],
		      int *exit_immediately, ElfType *elftype)
{
	int opt;
	int use_seccomp_filter = 0;
	int forward = 1;
	int binding = 0;
	int chroot = 0, pivot_root = 0;
	int mount_ns = 0, skip_remount = 0;
	int inherit_suppl_gids = 0, keep_suppl_gids = 0;
	int caps = 0, ambient_caps = 0;
	int seccomp = -1;
	const size_t path_max = 4096;
	uid_t uid = 0;
	gid_t gid = 0;
	char *uidmap = NULL, *gidmap = NULL;
	int set_uidmap = 0, set_gidmap = 0;
	size_t size;
	const char *filter_path = NULL;
	int log_to_stderr = 0;

	const char *optstring =
	    "+u:g:sS:c:C:P:b:B:V:f:m::M::k:a:e::R:T:vrGhHinNplLt::IUKwyYzd";
	/* clang-format off */
	const struct option long_options[] = {
		{"help", no_argument, 0, 'h'},
		{"mount-dev", no_argument, 0, 'd'},
		{"ambient", no_argument, 0, 128},
		{"uts", optional_argument, 0, 129},
		{"logging", required_argument, 0, 130},
		{0, 0, 0, 0},
	};
	/* clang-format on */

	while ((opt = getopt_long(argc, argv, optstring, long_options, NULL)) !=
	       -1) {
		switch (opt) {
		case 'u':
			set_user(j, optarg, &uid, &gid);
			break;
		case 'g':
			set_group(j, optarg, &gid);
			break;
		case 'n':
			minijail_no_new_privs(j);
			break;
		case 's':
			if (seccomp != -1 && seccomp != 1) {
				fprintf(stderr,
					"Do not use -s & -S together.\n");
				exit(1);
			}
			seccomp = 1;
			minijail_use_seccomp(j);
			break;
		case 'S':
			if (seccomp != -1 && seccomp != 2) {
				fprintf(stderr,
					"Do not use -s & -S together.\n");
				exit(1);
			}
			seccomp = 2;
			minijail_use_seccomp_filter(j);
			if (strlen(optarg) >= path_max) {
				fprintf(stderr, "Filter path is too long.\n");
				exit(1);
			}
			filter_path = strndup(optarg, path_max);
			if (!filter_path) {
				fprintf(stderr,
					"Could not strndup(3) filter path.\n");
				exit(1);
			}
			use_seccomp_filter = 1;
			break;
		case 'l':
			minijail_namespace_ipc(j);
			break;
		case 'L':
			minijail_log_seccomp_filter_failures(j);
			break;
		case 'b':
			add_binding(j, optarg);
			binding = 1;
			break;
		case 'B':
			skip_securebits(j, optarg);
			break;
		case 'c':
			caps = 1;
			use_caps(j, optarg);
			break;
		case 'C':
			if (pivot_root) {
				fprintf(stderr, "Could not set chroot because "
						"'-P' was specified.\n");
				exit(1);
			}
			if (0 != minijail_enter_chroot(j, optarg)) {
				fprintf(stderr, "Could not set chroot.\n");
				exit(1);
			}
			chroot = 1;
			break;
		case 'k':
			add_mount(j, optarg);
			break;
		case 'K':
			minijail_skip_remount_private(j);
			skip_remount = 1;
			break;
		case 'P':
			if (chroot) {
				fprintf(stderr,
					"Could not set pivot_root because "
					"'-C' was specified.\n");
				exit(1);
			}
			if (0 != minijail_enter_pivot_root(j, optarg)) {
				fprintf(stderr, "Could not set pivot_root.\n");
				exit(1);
			}
			minijail_namespace_vfs(j);
			pivot_root = 1;
			break;
		case 'f':
			if (0 != minijail_write_pid_file(j, optarg)) {
				fprintf(stderr,
					"Could not prepare pid file path.\n");
				exit(1);
			}
			break;
		case 't':
			minijail_namespace_vfs(j);
			size = 64 * 1024 * 1024;
			if (optarg != NULL && 0 != parse_size(&size, optarg)) {
				fprintf(stderr, "Invalid /tmp tmpfs size.\n");
				exit(1);
			}
			minijail_mount_tmp_size(j, size);
			break;
		case 'v':
			minijail_namespace_vfs(j);
			mount_ns = 1;
			break;
		case 'V':
			minijail_namespace_enter_vfs(j, optarg);
			break;
		case 'r':
			minijail_remount_proc_readonly(j);
			break;
		case 'G':
			if (keep_suppl_gids) {
				fprintf(stderr,
					"-y and -G are not compatible.\n");
				exit(1);
			}
			minijail_inherit_usergroups(j);
			inherit_suppl_gids = 1;
			break;
		case 'y':
			if (inherit_suppl_gids) {
				fprintf(stderr,
					"-y and -G are not compatible.\n");
				exit(1);
			}
			minijail_keep_supplementary_gids(j);
			keep_suppl_gids = 1;
			break;
		case 'N':
			minijail_namespace_cgroups(j);
			break;
		case 'p':
			minijail_namespace_pids(j);
			break;
		case 'e':
			if (optarg)
				minijail_namespace_enter_net(j, optarg);
			else
				minijail_namespace_net(j);
			break;
		case 'i':
			*exit_immediately = 1;
			break;
		case 'H':
			seccomp_filter_usage(argv[0]);
			exit(1);
		case 'I':
			minijail_namespace_pids(j);
			minijail_run_as_init(j);
			break;
		case 'U':
			minijail_namespace_user(j);
			minijail_namespace_pids(j);
			break;
		case 'm':
			set_uidmap = 1;
			if (uidmap) {
				free(uidmap);
				uidmap = NULL;
			}
			if (optarg)
				uidmap = strdup(optarg);
			break;
		case 'M':
			set_gidmap = 1;
			if (gidmap) {
				free(gidmap);
				gidmap = NULL;
			}
			if (optarg)
				gidmap = strdup(optarg);
			break;
		case 'a':
			if (0 != minijail_use_alt_syscall(j, optarg)) {
				fprintf(stderr,
					"Could not set alt-syscall table.\n");
				exit(1);
			}
			break;
		case 'R':
			add_rlimit(j, optarg);
			break;
		case 'T':
			if (!strcmp(optarg, "static"))
				*elftype = ELFSTATIC;
			else if (!strcmp(optarg, "dynamic"))
				*elftype = ELFDYNAMIC;
			else {
				fprintf(stderr, "ELF type must be 'static' or "
						"'dynamic'.\n");
				exit(1);
			}
			break;
		case 'w':
			minijail_new_session_keyring(j);
			break;
		case 'Y':
			minijail_set_seccomp_filter_tsync(j);
			break;
		case 'z':
			forward = 0;
			break;
		case 'd':
			minijail_namespace_vfs(j);
			minijail_mount_dev(j);
			break;
		/* Long options. */
		case 128: /* Ambient caps. */
			ambient_caps = 1;
			minijail_set_ambient_caps(j);
			break;
		case 129: /* UTS/hostname namespace. */
			minijail_namespace_uts(j);
			if (optarg)
				minijail_namespace_set_hostname(j, optarg);
			break;
		case 130: /* Logging. */
			if (!strcmp(optarg, "syslog"))
				log_to_stderr = 0;
			else if (!strcmp(optarg, "stderr")) {
				log_to_stderr = 1;
			} else {
				fprintf(stderr, "--logger must be 'syslog' or "
						"'stderr'.\n");
				exit(1);
			}
			break;
		default:
			usage(argv[0]);
			exit(opt == 'h' ? 0 : 1);
		}
	}

	if (log_to_stderr) {
		init_logging(LOG_TO_FD, STDERR_FILENO, LOG_INFO);
		/*
		 * When logging to stderr, ensure the FD survives the jailing.
		 */
		if (0 !=
		    minijail_preserve_fd(j, STDERR_FILENO, STDERR_FILENO)) {
			fprintf(stderr, "Could not preserve stderr.\n");
			exit(1);
		}
	}

	/* Set up uid/gid mapping. */
	if (set_uidmap || set_gidmap) {
		set_ugid_mapping(j, set_uidmap, uid, uidmap, set_gidmap, gid,
				 gidmap);
	}

	/* Can only set ambient caps when using regular caps. */
	if (ambient_caps && !caps) {
		fprintf(stderr, "Can't set ambient capabilities (--ambient) "
				"without actually using capabilities (-c).\n");
		exit(1);
	}

	/* Set up signal handlers in minijail unless asked not to. */
	if (forward)
		minijail_forward_signals(j);

	/*
	 * Only allow bind mounts when entering a chroot, using pivot_root, or
	 * a new mount namespace.
	 */
	if (binding && !(chroot || pivot_root || mount_ns)) {
		fprintf(stderr, "Bind mounts require a chroot, pivot_root, or "
				" new mount namespace.\n");
		exit(1);
	}

	/*
	 * Remounting / as MS_PRIVATE only happens when entering a new mount
	 * namespace, so skipping it only applies in that case.
	 */
	if (skip_remount && !mount_ns) {
		fprintf(stderr, "Can't skip marking mounts as MS_PRIVATE"
				" without mount namespaces.\n");
		exit(1);
	}

	/*
	 * We parse seccomp filters here to make sure we've collected all
	 * cmdline options.
	 */
	if (use_seccomp_filter) {
		minijail_parse_seccomp_filters(j, filter_path);
		free((void *)filter_path);
	}

	/*
	 * There should be at least one additional unparsed argument: the
	 * executable name.
	 */
	if (argc == optind) {
		usage(argv[0]);
		exit(1);
	}

	if (*elftype == ELFERROR) {
		/*
		 * -T was not specified.
		 * Get the path to the program adjusted for changing root.
		 */
		char *program_path =
		    minijail_get_original_path(j, argv[optind]);

		/* Check that we can access the target program. */
		if (access(program_path, X_OK)) {
			fprintf(stderr,
				"Target program '%s' is not accessible.\n",
				argv[optind]);
			exit(1);
		}

		/* Check if target is statically or dynamically linked. */
		*elftype = get_elf_linkage(program_path);
		free(program_path);
	}

	/*
	 * Setting capabilities need either a dynamically-linked binary, or the
	 * use of ambient capabilities for them to be able to survive an
	 * execve(2).
	 */
	if (caps && *elftype == ELFSTATIC && !ambient_caps) {
		fprintf(stderr, "Can't run statically-linked binaries with "
				"capabilities (-c) without also setting "
				"ambient capabilities. Try passing "
				"--ambient.\n");
		exit(1);
	}

	return optind;
}

int main(int argc, char *argv[])
{
	struct minijail *j = minijail_new();
	const char *dl_mesg = NULL;
	int exit_immediately = 0;
	ElfType elftype = ELFERROR;
	int consumed = parse_args(j, argc, argv, &exit_immediately, &elftype);
	argc -= consumed;
	argv += consumed;

	/*
	 * Make the process group ID of this process equal to its PID.
	 * In the non-interactive case (e.g. when minijail0 is started from
	 * init) this ensures the parent process and the jailed process
	 * can be killed together.
	 *
	 * Don't fail on EPERM, since setpgid(0, 0) can only EPERM when
	 * the process is already a process group leader.
	 */
	if (setpgid(0 /* use calling PID */, 0 /* make PGID = PID */)) {
		if (errno != EPERM) {
			fprintf(stderr, "setpgid(0, 0) failed\n");
			exit(1);
		}
	}

	if (elftype == ELFSTATIC) {
		/*
		 * Target binary is statically linked so we cannot use
		 * libminijailpreload.so.
		 */
		minijail_run_no_preload(j, argv[0], argv);
	} else if (elftype == ELFDYNAMIC) {
		/*
		 * Target binary is dynamically linked so we can
		 * inject libminijailpreload.so into it.
		 */

		/* Check that we can dlopen() libminijailpreload.so. */
		if (!dlopen(PRELOADPATH, RTLD_LAZY | RTLD_LOCAL)) {
			dl_mesg = dlerror();
			fprintf(stderr, "dlopen(): %s\n", dl_mesg);
			return 1;
		}
		minijail_run(j, argv[0], argv);
	} else {
		fprintf(stderr,
			"Target program '%s' is not a valid ELF file.\n",
			argv[0]);
		return 1;
	}

	if (exit_immediately) {
		info("not running init loop, exiting immediately\n");
		return 0;
	}
	int ret = minijail_wait(j);
#if defined(__SANITIZE_ADDRESS__)
	minijail_destroy(j);
#endif /* __SANITIZE_ADDRESS__ */
	return ret;
}
