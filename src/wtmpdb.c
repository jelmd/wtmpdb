/* SPDX-License-Identifier: BSD-2-Clause

  Copyright (c) 2023, Thorsten Kukuk <kukuk@suse.com>,
  Portions Copyright (c) 2025 by Jens Elkner <jel+wtmpdb@cs.ovgu.de>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <getopt.h>
#include <netdb.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <sys/utsname.h>
#include <assert.h>

#if HAVE_AUDIT
#include <libaudit.h>
#endif

#if HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#define _cleanup_(f) __attribute__((cleanup(f)))
#endif

#include "import.h"
#include "wtmpdb.h"

static char *wtmpdb_path = NULL;

#define TIMEFMT_CTIME  1
#define TIMEFMT_SHORT  2
#define TIMEFMT_HHMM   3
#define TIMEFMT_NOTIME 4
#define TIMEFMT_ISO    5
#define TIMEFMT_COMPACT 6
#define TIMEFMT_MAX 7
/* Helper to map constants above to the proper fmtime string*/
static const char *TM_FMT_SPEC[] = {
	"%s",
	"%s",
	"%a %b %e %H:%M",
	"%H:%M",
	"%s",
	"%FT%T%z",	/* Same ISO8601 format original last command uses */
	"%F %T",
	NULL
};

#define TIMEFMT_VALUE 255

#define LOGROTATE_DAYS 60

/* length of login string cannot become longer */
#define LAST_TIMESTAMP_LEN 32

static uint64_t wtmp_start = UINT64_MAX;
static uint64_t time_now = 0;

/* options for last */
static int hostlast = 0;
static int nohostname = 0;
static int noservice = 1;
static int dflag = 0;
static int iflag = 0;
static int jflag = 0;
static int wflag = 0;
static int xflag = 0;
static int legacy = 0;
static int uniq = 0;
static int compact = 0;
static int open_sessions = 0; /* show open sessions, only */
static const int name_len = 8; /* LAST_LOGIN_LEN */
static int login_fmt = TIMEFMT_SHORT;
static int login_len = 16; /* 16 = short, 24 = full */
static int logout_fmt = TIMEFMT_HHMM;
static int logout_len = 5; /* 5 = short, 24 = full */
static const int host_len = 16; /* LAST_DOMAIN_LEN */
static unsigned long maxentries = 0; /* max number of entries to show */
static unsigned long currentry = 0; /* number of entries already printed */
static uint64_t present = 0; /* Who was present at the specified time in µs? */
static uint64_t since = 0; /* Who was logged in after this time in µs? */
static uint64_t until = 0; /* Who was logged in until this time in µs? */
static char **match = NULL; /* user/tty to display only */

typedef enum cmd_idx {
	CMD_NONE = 0,
	CMD_LAST,
	CMD_BOOT,
	CMD_SHUTDOWN,
	CMD_BOOTTIME,
	CMD_ROTATE,
	CMD_IMPORT,
	CMD_MAX			/* per contract the always the last one */
} cmd_idx_t;

static const char *cmd_name[] = {
	"unknown",	"last",		"boot",		"shutdown",		"boottime",
	"rotate",	"import",	NULL
};


/* isipaddr - find out if string provided is an IP address or not
   0 - no IP address
   1 - is IP address
*/
static int
isipaddr (const char *string, int *addr_type,
          struct sockaddr_storage *addr)
{
  struct sockaddr_storage local_addr;
  int is_ip;

  if (addr == NULL)
    addr = &local_addr;

  memset(addr, 0, sizeof (struct sockaddr_storage));

  /* first ipv4 */
  if (inet_pton (AF_INET, string, &((struct sockaddr_in *)addr)->sin_addr) > 0)
    {
      if (addr_type != NULL)
        *addr_type = AF_INET;
      addr->ss_family = AF_INET;
      is_ip = 1;
    }
  else if (inet_pton (AF_INET6, string, &((struct sockaddr_in6 *)addr)->sin6_addr) > 0)
    { /* then ipv6 */
      if (addr_type != NULL)
        *addr_type = AF_INET6;
      addr->ss_family = AF_INET6;
      is_ip = 1;
    }
  else
    is_ip = 0;

  return is_ip;
}

static int
parse_time (const char *str, uint64_t *microseconds)
{
  const char *abs_datetime_fmts[] = {
    "%Y%m%d%H%M%S",
    "%Y-%m-%d %T",
    "%Y-%m-%d %R",
    "%Y-%m-%d",
    NULL
  };
  const char *abs_time_fmts[] = {
    "%T",
    "%R",
    NULL
  };
  const char **fmt;
  struct tm rst = { .tm_isdst = -1 };
  struct tm res;
  char *r = NULL;
  time_t t = time (NULL);

  for (fmt = abs_datetime_fmts; *fmt; fmt++)
    {
      res = rst;
      r = strptime (str, *fmt, &res);
      if (r != NULL && *r == '\0') goto done;
    }

  /* Use today's date for time-only specs */
  localtime_r (&t, &rst);
  rst.tm_isdst = -1;

  for (fmt = abs_time_fmts; *fmt; fmt++)
    {
      res = rst;
      r = strptime (str, *fmt, &res);
      if (r != NULL && *r == '\0') goto done;
    }

  if (strcmp (str, "now") == 0)
    goto done;

  /* Reset time of day for date-only specs */
  res.tm_sec = res.tm_min = res.tm_hour = 0;

  if (strcmp (str, "yesterday") == 0)
    res.tm_mday--;
  else if (strcmp (str, "tomorrow") == 0)
    res.tm_mday++;
  else if (strcmp (str, "today") != 0)
    return -1;

done:
  *microseconds = mktime (&res) * USEC_PER_SEC;

  return 0;
}

static int
time_format (const char *fmt)
{
  if (strcmp (fmt, "notime") == 0)
    {
      login_fmt = TIMEFMT_NOTIME;
      login_len = 0;
      logout_fmt = TIMEFMT_NOTIME;
      logout_len = 0;
      return TIMEFMT_NOTIME;
    }
  if (strcmp (fmt, "short") == 0)
    {
      login_fmt = TIMEFMT_SHORT;
      login_len = 16;
      logout_fmt = TIMEFMT_HHMM;
      logout_len = 5;
      return TIMEFMT_SHORT;
    }
  if (strcmp (fmt, "full") == 0)
   {
     login_fmt = TIMEFMT_CTIME;
     login_len = 24;
     logout_fmt = TIMEFMT_CTIME;
     logout_len = 24;
     return TIMEFMT_CTIME;
   }
  if (strcmp (fmt, "iso") == 0)
   {
     login_fmt = TIMEFMT_ISO;
     login_len = 25;
     logout_fmt = TIMEFMT_ISO;
     logout_len = 25;
     return TIMEFMT_ISO;
   }
  if (strcmp (fmt, "compact") == 0)
   {
     login_fmt = TIMEFMT_COMPACT;
     login_len = 19;
     logout_fmt = TIMEFMT_COMPACT;
     logout_len = 19;	/* set to 0 if output mode is compact, i.e. no logout times */
     return TIMEFMT_COMPACT;
   }

  return -1;
}

static void
format_time (int fmt, char *dst, size_t dstlen, uint64_t microseconds)
{
	time_t t = (time_t) (microseconds / USEC_PER_SEC);
	if (fmt == TIMEFMT_CTIME) {
		snprintf (dst, dstlen, "%s", ctime (&t));
		dst[strlen (dst)-1] = '\0'; /* Remove trailing '\n' */
		return;
	} else if (fmt == TIMEFMT_NOTIME) {
		*dst = '\0';
		return;
	}
	assert(fmt > 0 && fmt < TIMEFMT_MAX);
	const char *tm_fmt = TM_FMT_SPEC[fmt];
	struct tm *tm = localtime (&t);
	strftime (dst, dstlen, tm_fmt, tm);
}

static void
calc_time_length(char *dst, size_t dstlen, uint64_t start, uint64_t stop, char prefix)
{
  uint64_t secs = (stop - start)/USEC_PER_SEC;
  int mins  = (secs / 60) % 60;
  int hours = (secs / 3600) % 24;
  uint64_t days  = secs / 86400;

  if (!legacy) {
    secs %= 60;
    if (days)
      snprintf (dst, dstlen, "%c(%" PRId64 "+%02d:%02d:%02lu)", prefix, days, hours, mins, secs);
    else if (hours)
      snprintf (dst, dstlen, "%c(%02d:%02d:%02lu)", prefix, hours, mins, secs);
    else
      snprintf (dst, dstlen, "%c(00:%02d:%02lu)", prefix, mins, secs);
	return;
  }
  if (days)
    snprintf (dst, dstlen, "%c(%" PRId64 "+%02d:%02d)", prefix, days, hours, mins);
  else if (hours)
    snprintf (dst, dstlen, "%c(%02d:%02d)", prefix, hours, mins);
  else
    snprintf (dst, dstlen, "%c(00:%02d)", prefix, mins);
}

/* map "soft-reboot" to "s-reboot" if we have only 8 characters
   for user output (no -w specified) */
static const char *
map_soft_reboot (const char *user)
{
  if (wflag || strcmp (user, "soft-reboot") != 0)
    return user;

  if ((int)strlen (user) > name_len)
    return "s-reboot";

  return user;
}

static const char *
remove_parentheses(const char *str)
{

  static char buf[LAST_TIMESTAMP_LEN];

  if (strlen(str) >= LAST_TIMESTAMP_LEN)
    return str;

  char *cp = strchr (str, '(');

  if (cp == NULL)
    return str;

  cp++;
  strncpy(buf, cp, LAST_TIMESTAMP_LEN);

  cp = strchr (buf, ')');
  if (cp)
    *cp = '\0';

  return buf;
}

static int first_entry = 1;
static void
print_line (const char *user, const char *tty, const char *host,
	    const char *print_service,
	    const char *logintime, const char *logouttime,
	    const char *length)
{
  if (jflag)
    {
      if (first_entry)
	first_entry = 0;
      else
	printf (",\n");
      printf ("     { \"user\": \"%s\",\n", user);
      printf ("       \"tty\": \"%s\",\n", tty);
      if (!nohostname)
	printf ("       \"hostname\": \"%s\",\n", host);
      if (print_service && strlen (print_service) > 0)
	printf ("       \"service\": \"%s\",\n", print_service);
      printf ("       \"login\": \"%s\",\n", logintime);
		if (!compact)
	  printf ("       \"logout\": \"%s\",\n", logouttime);
      if (length[0] == ' ' || length[0] == '(')
	{
	  printf ("       \"length\": \"%s\"\n",  remove_parentheses(length));
	}
      else
	printf ("       \"length\": \"%s\"\n", length);
      printf ("     }");
    }
  else
    {
      char *line;
	  const char *sep = compact ? "" : " - ";

      if (nohostname)
	{
	  if (asprintf (&line, "%-8.*s %-12.12s%s %-*.*s%s%-*.*s %s\n",
			wflag?(int)strlen (user):name_len,
			map_soft_reboot (user), tty, print_service,
			login_len, login_len, logintime, sep,
			logout_len, logout_len, logouttime,
			length) < 0)
	    {
	      fprintf (stderr, "Out f memory");
	      exit (EXIT_FAILURE);
	    }
	}
      else
	{
	  if (hostlast)
	    {
	      if (asprintf (&line, "%-8.*s %-12.12s%s %-*.*s%s%-*.*s %-12.12s %s\n",
			    wflag?(int)strlen(user):name_len, map_soft_reboot (user),
			    tty, print_service,
			    login_len, login_len, logintime, sep,
			    logout_len, logout_len, logouttime,
			    length, host) < 0)
		{
		  fprintf (stderr, "Out f memory");
		  exit (EXIT_FAILURE);
		}
	    }
	  else
	    {
	      if (asprintf (&line, "%-8.*s %-12.12s %-16.*s%s %-*.*s%s%-*.*s %s\n",
			    wflag?(int)strlen(user):name_len, map_soft_reboot (user), tty,
			    wflag?(int)strlen(host):host_len, host, print_service,
			    login_len, login_len, logintime, sep,
			    logout_len, logout_len, logouttime,
			    length) < 0)
		{
		  fprintf (stderr, "Out f memory");
		  exit (EXIT_FAILURE);
		}
	    }
	}

      printf ("%s", line);
      free (line);
    }
}

static void
dump_entry(int argc, char **argv, char **azColName) {
	for (int i = 0; i < argc; i++)
		fprintf (stderr, " %s='%s'", azColName[i], argv[i] ? argv[i] : "NULL");
	fprintf (stderr, "\n");
}

#define UPDATE_LAST_BOOT_TIME \
	if (type == BOOT_TIME) {\
		if (login_t < last_reboot) \
			last_reboot = login_t;\
		tty = "system boot";\
	}

static int
print_entry(void *unused __attribute__((__unused__)),
	int argc, char **argv, char **azColName)
{
	static uint64_t last_reboot = UINT64_MAX;

	char host_buf[NI_MAXHOST];
	struct times_buf {
		char login[LAST_TIMESTAMP_LEN];
		char logout[LAST_TIMESTAMP_LEN];
		char length[LAST_TIMESTAMP_LEN];
	} times;
	char *endptr;
	uint64_t logout_t = 0;
	int has_logout = argv[4] != NULL;

	/* Yes, it's waste of time to let sqlite iterate through all entries
	   even if we don't need more anymore, but telling sqlite we don't
	   want more leads to a "query aborted" error... */
	if (maxentries && currentry >= maxentries)
		return 0;

	/* ID, Type, User, LoginTime, LogoutTime, TTY, RemoteHost, Service */
	if (argc != 8) {
		fprintf(stderr, "Mangled entry:");
		dump_entry(argc, argv, azColName);
		exit(EXIT_FAILURE);
	}

	const int type = atoi(argv[1]);
	const char *user = argv[2];
	const char *tty = argv[5] ? argv[5] : "?";
	const char *host = argv[6] ? argv[6] : "";
	const char *service = argv[7] ? argv[7] : "";

	uint64_t login_t = strtoull(argv[3] ? argv[3] : "", &endptr, 10);
	if ((errno == ERANGE && login_t == ULLONG_MAX) || (endptr == argv[3])
		|| (*endptr != '\0'))
	{
		fprintf(stderr, "%s: Invalid numeric time entry for 'login': '%s'\n",
			argv[0], argv[3]);
		return 0;
	}
	if (login_t < wtmp_start)
		wtmp_start = login_t;

	if ((since && (login_t < since)) || (until && (login_t > until))
		|| (present && (present < login_t)))
	{
		UPDATE_LAST_BOOT_TIME
		return 0;
	}

	if (has_logout) {
		logout_t = strtoull(argv[4], &endptr, 10);
		if ((errno == ERANGE && logout_t == ULLONG_MAX) || (endptr == argv[4])
			|| (*endptr != '\0'))
		{
			fprintf(stderr, "%s: Invalid numeric time entry for 'logout': '%s'\n",
				argv[0], argv[4]);
			return 0;
		}
		if (logout_t > last_reboot)
			logout_t = last_reboot;
	} else {
		logout_t = last_reboot;
	}
	if (present && logout_t < present) {
		UPDATE_LAST_BOOT_TIME
		return 0;
	}

	if (match) {
		char **walk;

		for (walk = match; *walk; walk++) {
			if (strcmp(user, *walk) == 0 || strcmp(tty, *walk) == 0)
				break;
		}
		if (*walk == NULL)
			return 0;
	}

	format_time(login_fmt, times.login, sizeof(times.login), login_t);

	if (has_logout) {
		if (open_sessions) {
			UPDATE_LAST_BOOT_TIME
			return 0;
		}
		if (!compact)
			format_time(logout_fmt, times.logout, sizeof(times.logout), logout_t);
		calc_time_length(times.length, sizeof(times.length), login_t, logout_t, ' ');
	} else {
		/* login but no logout */
		if (compact) {
			if (last_reboot == UINT64_MAX) {
				calc_time_length(times.length, sizeof(times.length), login_t, time_now, '.');
			} else {
				calc_time_length(times.length, sizeof(times.length), login_t, last_reboot, '?');
			}
		} else if (last_reboot != UINT64_MAX) {
			snprintf(times.logout, sizeof(times.logout), "crash");
			times.length[0] = '\0';
		} else {
			switch (type)
			{
			case USER_PROCESS:
				if (logout_fmt == TIMEFMT_HHMM) {
					snprintf(times.logout, sizeof(times.logout), "still");
					snprintf(times.length, sizeof(times.length), "logged in");
				} else {
					snprintf(times.logout, sizeof(times.logout), "still logged in");
					times.length[0] = '\0';
				}
				break;
			case BOOT_TIME:
				if (logout_fmt == TIMEFMT_HHMM) {
					snprintf(times.logout, sizeof(times.logout), "still");
					snprintf(times.length, sizeof(times.length), "running");
				} else {
					snprintf(times.logout, sizeof(times.logout), "still running");
					times.length[0] = '\0';
				}
				break;
			default:
				snprintf(times.logout, sizeof(times.logout), "ERROR");
				snprintf(times.length, sizeof(times.length), "Unknown: %d", type);
				break;
			}
		}
	}

	char *print_service = NULL;
	if (noservice) {
		print_service = strdup("");
	} else if (asprintf(&print_service, " %-12.12s", service) < 0) {
		fprintf(stderr, "Out f memory");
		exit(EXIT_FAILURE);
	}

	if (dflag && strlen(host) > 0) {
		struct sockaddr_storage addr;
		int addr_type = 0;

		if (isipaddr(host, &addr_type, &addr)
			&& getnameinfo((struct sockaddr *)&addr, sizeof(addr), host_buf,
				sizeof(host_buf), NULL, 0, NI_NAMEREQD) == 0)
		{
			host = host_buf;
		}
	}

	if (iflag && strlen(host) > 0) {
		struct addrinfo hints, *result;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;	/* Allow IPv4 or IPv6 */
		hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
		hints.ai_flags = 0;
		hints.ai_protocol = 0; /* Any protocol */
		if (getaddrinfo(host, NULL, &hints, &result) == 0) {
			if (result->ai_family == AF_INET
				&& inet_ntop(result->ai_family,
					&((struct sockaddr_in *)result->ai_addr)->sin_addr,
					host_buf, sizeof(host_buf)) != NULL)
			{
				host = host_buf;
			} else if (result->ai_family == AF_INET6
				&& inet_ntop(result->ai_family,
					&((struct sockaddr_in6 *)result->ai_addr)->sin6_addr,
					host_buf, sizeof(host_buf)) != NULL)
			{
				host = host_buf;
			}
			freeaddrinfo(result);
		}
	}

	if (xflag && (type == BOOT_TIME) && last_reboot != UINT64_MAX && has_logout) {
		/* A little bit odd because this function is expected to be applied to
			a record list ordered by login_t, at least if boot record selection
			is enabled. So the best we can do is to inject a shutdown record
			related to the current entry, which usually breaks the chronolog.
			output wrt. login time. So either the user post-processes the output
			to get a less confusing output, or main_last() needs to do it. For
			now, we leave it to the user.
		*/
		struct times_buf shutdown;

		format_time(login_fmt, shutdown.login, sizeof(shutdown.login), logout_t);
		format_time(logout_fmt, shutdown.logout, sizeof(shutdown.logout), last_reboot);
		calc_time_length(shutdown.length, sizeof(shutdown.length), logout_t, last_reboot, ' ');

		print_line("shutdown", "system down", host, print_service,
			shutdown.login, shutdown.logout, shutdown.length);
	}
	UPDATE_LAST_BOOT_TIME
	print_line(user, tty, host, print_service, times.login, times.logout, times.length);

	free(print_service);
	currentry++;
	return 0;
}

static void
show_version(void)
{
  printf ("wtmpdb %s\n", VERSION);
  exit(EXIT_SUCCESS);
}

static void
usage (int retval, cmd_idx_t cmd)
{
  FILE *output = (retval != EXIT_SUCCESS) ? stderr : stdout;
  int i;

  if (cmd == CMD_NONE) {
    fprintf (output, "Usage: wtmpdb [command] [options] [operand]\n");
    fprintf (output, "\nCommands: %s", cmd_name[1]);
    for (i=2; i < CMD_MAX; i++)
      fprintf(output, ", %s", cmd_name[i]);
    fputs("\n\nCommon options:\n", output);
  } else {
    fprintf (output, "Usage: wtmpdb %s [options]%s\n", cmd_name[cmd],
		(cmd == CMD_LAST || cmd == CMD_IMPORT) ? " [operand]" : "");
    fputs ("\nOptions:\n", output);
  }
  fputs ("  -f, --file FILE     Use FILE as wtmpdb database\n", output);
  fputs ("  -h, --help          Display this help message and exit\n", output);
  fputs ("  -v, --version       Print version number and exit\n", output);
  if (cmd == CMD_NONE)
    fprintf (output, "\nOptions for %s:\n", cmd_name[CMD_LAST]);
  if (cmd == CMD_LAST || cmd == CMD_NONE) {
  fputs ("  -a, --hostlast      Display hostnames as last entry\n", output);
  fputs ("  -c, --compact       Hide logouts and set login time format to 'compact'\n", output);
  fputs ("  -d, --dns           Translate IP addresses into a hostname\n", output);
  fputs ("  -F, --fulltimes     Display full times and dates\n", output);
  fputs ("  -i, --ip            Translate hostnames to IP addresses\n", output);
  fputs ("  -j, --json          Generate JSON output\n", output);
  fputs ("  -L, --legacy        Session duration precision in minutes instead of seconds\n", output);
  fputs ("  -n, --limit N, -N   Display only first N entries\n", output);
  fputs ("  -o, --open          Display open sessions, only.\n", output);
  fputs ("  -p, --present TIME  Display who was present at TIME\n", output);
  fputs ("  -R, --nohostname    Don't display hostname\n", output);
  fputs ("  -S, --service       Display PAM service used to login\n", output);
  fputs ("  -s, --since TIME    Display who was logged in after TIME\n", output);
  fputs ("  -t, --until TIME    Display who was logged in until TIME\n", output);
  fputs ("  -u, --unique        Display the latest entry for each user, only.\n", output);
  fputs ("  -w, --fullnames     Display full IP addresses and user and domain names\n", output);
  fputs ("  -x, --system        Display system shutdown entries\n", output);
    fputs ("  --time-format FMT   Display timestamps in the specified format.\n", output);
    fputs ("\n  FMT format: notime|short|full|iso|compact\n", output);
    fputs ("  TIME format: YYYY-MM-DD HH:MM:SS\n", output);
  }
  if (cmd == CMD_NONE)
    fprintf (output, "\nOperands for %s:\n", cmd_name[CMD_LAST]);
  if (cmd == CMD_LAST) {
    fputs ("\nOperands:\n", output);
  }
  if (cmd == CMD_LAST || cmd == CMD_NONE) {
    fputs ("  username...         Display only entries matching these arguments\n", output);
    fputs ("  tty...              Display only entries matching these arguments\n", output);
  }
  if (cmd == CMD_NONE)
    fputs ("\nOptions for rotate (exports old entries to wtmpdb_<datetime>)):\n", output);
  if (cmd == CMD_NONE || cmd == CMD_ROTATE) {
  fputs ("  -d, --days INTEGER  Export all entries which are older than the given days\n", output);
  }
  if (cmd == CMD_NONE)
    fprintf (output, "\nOperands for %s:\n", cmd_name[CMD_IMPORT]);
  if (cmd == CMD_IMPORT)
    fputs ("\nOperands:\n", output);
  if (cmd == CMD_IMPORT || cmd == CMD_NONE)
  fputs ("  logs...             Legacy log files to import\n", output);
  exit (retval);
}

static int
main_rotate (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {"file", required_argument, NULL, 'f'},
    {"days", no_argument, NULL, 'd'},
    {NULL, 0, NULL, '\0'}
  };
  char *error = NULL;
  int days = LOGROTATE_DAYS;
  char *wtmpdb_backup = NULL;
  uint64_t entries = 0;

  int c;

  while ((c = getopt_long (argc, argv, "f:d:hv", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'f':
          wtmpdb_path = optarg;
          break;
	case 'd':
	  days = atoi (optarg);
	  break;
        case 'v':
          show_version();
          break;
        case 'h':
          usage (EXIT_SUCCESS, CMD_ROTATE);
          break;
        default:
          usage (EXIT_FAILURE, CMD_ROTATE);
          break;
        }
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE, CMD_ROTATE);
    }

  if (wtmpdb_rotate (wtmpdb_path, days, &error,
		     &wtmpdb_backup, &entries) != 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't read all wtmp entries\n");

      exit (EXIT_FAILURE);
    }

  if (entries == 0 || wtmpdb_backup == NULL)
    printf ("No old entries found\n");
  else
    printf ("%lli entries moved to %s\n",
	    (long long unsigned int)entries, wtmpdb_backup);

  free (wtmpdb_backup);

  return EXIT_SUCCESS;
}

static int
main_last (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {"hostlast", no_argument, NULL, 'a'},
    {"compact", no_argument, NULL, 'c'},
    {"dns", no_argument, NULL, 'd'},
    {"file", required_argument, NULL, 'f'},
    {"fullnames", no_argument, NULL, 'w'},
    {"fulltimes", no_argument, NULL, 'F'},
    {"ip", no_argument, NULL, 'i'},
    {"legacy", no_argument, NULL, 'L'},
    {"limit", required_argument, NULL, 'n'},
    {"open", no_argument, NULL, 'o'},
    {"present", required_argument, NULL, 'p'},
    {"nohostname", no_argument, NULL, 'R'},
    {"service", no_argument, NULL, 'S'},
    {"since", required_argument, NULL, 's'},
    {"system", no_argument, NULL, 'x'},
    {"unique", no_argument, NULL, 'u'},
    {"until", required_argument, NULL, 't'},
    {"time-format", required_argument, NULL, TIMEFMT_VALUE},
    {"json", no_argument, NULL, 'j'},
    {NULL, 0, NULL, '\0'}
  };
  int time_fmt = TIMEFMT_CTIME;
  char *error = NULL;
  int c;

  if (getenv("LAST_COMPACT")) {
	  compact = 1;
	  time_fmt = time_format("compact"); /* We allow to overwrite login_t fmt */
	  logout_len = 0;
  }

  while ((c = getopt_long (argc, argv, "0123456789acdf:FhijLn:op:RSs:t:uvwx",
			   longopts, NULL)) != -1)
    {
      switch (c)
        {
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	  maxentries = maxentries * 10 + c - '0';
	  break;
	case 'a':
	  hostlast = 1;
	  break;
	case 'c':
		compact = 1;
		time_fmt = time_format("compact"); /* We allow to overwrite login_t fmt */
		logout_len = 0;
		break;
	case 'd':
		dflag = 1;
		break;
	case 'f':
		wtmpdb_path = optarg;
		break;
	case 'F':
	  login_fmt = TIMEFMT_CTIME;
	  login_len = 24;
	  logout_fmt = TIMEFMT_CTIME;
	  logout_len = 24;
	  compact = 0;
	  break;
	case 'i':
	  iflag = 1;
	  break;
	case 'j':
	  jflag = 1;
	  break;
	case 'L':
	  legacy = 1;
	  break;
	case 'n':
	  maxentries = strtoul (optarg, NULL, 10);
	  break;
	case 'o':
	  open_sessions = 1;
	  break;
	case 'p':
	  if (parse_time (optarg, &present) < 0)
	    {
	      fprintf (stderr, "Invalid time value '%s'\n", optarg);
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'R':
	  nohostname = 1;
	  break;
	case 's':
	  if (parse_time (optarg, &since) < 0)
	    {
	      fprintf (stderr, "Invalid time value '%s'\n", optarg);
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'S':
	  noservice = 0;
	  break;
	case 't':
	  if (parse_time (optarg, &until) < 0)
	    {
	      fprintf (stderr, "Invalid time value '%s'\n", optarg);
	      exit (EXIT_FAILURE);
	    }
	  break;
	case 'u':
	  uniq = 1;
	  break;
	case 'w':
	  wflag = 1;
	  break;
	case 'x':
	  xflag = 1;
	  break;
	case TIMEFMT_VALUE:
	  time_fmt = time_format (optarg);
	  if (time_fmt == -1)
	    {
	      fprintf (stderr, "Invalid time format '%s'\n", optarg);
	      exit (EXIT_FAILURE);
	    }
	  break;
        case 'v':
          show_version();
          break;
        case 'h':
          usage (EXIT_SUCCESS, CMD_LAST);
          break;
        default:
          usage (EXIT_FAILURE, CMD_LAST);
          break;
        }
    }

  if (argc > optind)
    match = argv + optind;

  if (compact) {
	  logout_len = 0;
	  parse_time("now", &time_now);
  }

  if (nohostname && hostlast)
    {
      fprintf (stderr, "The options -a and -R cannot be used together.\n");
      usage (EXIT_FAILURE, CMD_LAST);
    }

  if (nohostname && dflag)
    {
      fprintf (stderr, "The options -d and -R cannot be used together.\n");
      usage (EXIT_FAILURE, CMD_LAST);
    }

  if (nohostname && iflag)
    {
      fprintf (stderr, "The options -i and -R cannot be used together.\n");
      usage (EXIT_FAILURE, CMD_LAST);
    }

  if (dflag && iflag)
    {
      fprintf (stderr, "The options -d and -i cannot be used together.\n");
      usage (EXIT_FAILURE, CMD_LAST);
    }

	if (present != 0) {
		if (since != 0 && present < since)
			return EXIT_SUCCESS;
		if (until != 0) {
			if (present > until)
				return EXIT_SUCCESS;
			until = present;
		}
	}
	if (since != 0 && until != 0 && since > until)
		return EXIT_SUCCESS;

	if (jflag)
		printf("{\n   \"entries\": [\n");

	if (wtmpdb_read_all(wtmpdb_path, uniq, print_entry, &error) != 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't read all wtmp entries\n");

      exit (EXIT_FAILURE);
    }

  if (wtmp_start == UINT64_MAX)
    {
      if (!jflag)
	printf ("%s has no entries\n", wtmpdb_path?wtmpdb_path:"wtmpdb");
    }
  else if (time_fmt != TIMEFMT_NOTIME)
    {
      char wtmptime[32];
      format_time (time_fmt, wtmptime, sizeof(wtmptime), wtmp_start);
      if (jflag)
	printf ("\n   ],\n   \"start\": \"%s\"\n", wtmptime);
      else
	printf ("\n%s begins %s\n", wtmpdb_path?wtmpdb_path:"wtmpdb", wtmptime);
    }
  else if (jflag)
    printf ("\n   ]\n");

  if (jflag)
    printf ("}\n");
  return EXIT_SUCCESS;
}

#if HAVE_AUDIT
static void
log_audit (int type)
{
  int audit_fd = audit_open();

  if (audit_fd < 0)
    {
      fprintf (stderr, "Failed to connect to audit daemon: %s\n",
	       strerror (errno));
      return;
    }

  if (audit_log_user_comm_message(audit_fd, type, "", "wtmpdb", NULL, NULL, NULL, 1) < 0)
    fprintf (stderr, "Failed to send audit message: %s",
	     strerror (errno));
  audit_close (audit_fd);
}
#endif

static struct timespec
diff_timespec(const struct timespec *time1, const struct timespec *time0)
{
  struct timespec diff = {.tv_sec = time1->tv_sec - time0->tv_sec,
    .tv_nsec = time1->tv_nsec - time0->tv_nsec};
  if (diff.tv_nsec < 0) {
    diff.tv_nsec += 1000000000; // nsec/sec
    diff.tv_sec--;
  }
  return diff;
}

#if HAVE_SYSTEMD
/* Find out if it was a soft-reboot. With systemd v256 we can query systemd
   for this.
   Return values:
   -1: no systemd support
   0: no soft-reboot
   >0: number of soft-reboots
*/
static int
soft_reboots_count (void)
{
  unsigned soft_reboots_count = -1;
  _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int r;

  if (sd_bus_open_system (&bus) < 0)
    {
      return -1;
    }

  r = sd_bus_get_property_trivial (bus, "org.freedesktop.systemd1",
				   "/org/freedesktop/systemd1",
				   "org.freedesktop.systemd1.Manager",
				   "SoftRebootsCount",
				   &error, 'u', &soft_reboots_count);
  if (r < 0)
    {
      sd_bus_error_free (&error);
      return -1;
    }
  return soft_reboots_count;
}
#endif

static int
main_boot (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {"file", required_argument, NULL, 'f'},
    {"quiet", no_argument, NULL, 'q'},
    {NULL, 0, NULL, '\0'}
  };
  char *error = NULL;
  int c;
  int soft_reboot = 0;
#if HAVE_SYSTEMD
  int quiet = 0;
#endif

  while ((c = getopt_long (argc, argv, "f:hqv", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'f':
          wtmpdb_path = optarg;
          break;
	case 'q':
#if HAVE_SYSTEMD
	  quiet = 1;
#endif
	  break;
        case 'v':
          show_version();
          break;
        case 'h':
          usage (EXIT_SUCCESS, CMD_BOOT);
          break;
        default:
          usage (EXIT_FAILURE, CMD_BOOT);
          break;
        }
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE, CMD_BOOT);
    }

  struct utsname uts;
  uname(&uts);

  struct timespec ts_now;
  struct timespec ts_boot;
  clock_gettime (CLOCK_REALTIME, &ts_now);
#ifdef CLOCK_BOOTTIME
  clock_gettime (CLOCK_BOOTTIME, &ts_boot);
#else
  ts_boot = ts_now;
#endif
  uint64_t time = wtmpdb_timespec2usec (diff_timespec(&ts_now, &ts_boot));
#if HAVE_SYSTEMD
  struct timespec ts_empty = { .tv_sec = 0, .tv_nsec = 0 };
  uint64_t now = wtmpdb_timespec2usec (diff_timespec(&ts_now, &ts_empty));

  int count = soft_reboots_count ();

  if (count > 0)
    {
      time = now;
      soft_reboot = 1;
    }
  else if ((count < 0) && ((now - time) > 300 * USEC_PER_SEC) /* 5 minutes */)
    {
      if (!quiet)
	{
	  char timebuf[32];
	  printf ("Boot time too far in the past, using current time:\n");
	  format_time (TIMEFMT_CTIME, timebuf, sizeof(timebuf), time);
	  printf ("Boot time: %s\n", timebuf);
	  format_time (TIMEFMT_CTIME, timebuf, sizeof(timebuf), now);
	  printf ("Current time: %s\n", timebuf);
	}
      time = now;
      soft_reboot = 1;
    }
#endif

#if HAVE_AUDIT
  log_audit (AUDIT_SYSTEM_BOOT);
#endif

  if (wtmpdb_login (wtmpdb_path, BOOT_TIME, soft_reboot ? "soft-reboot" : "reboot", time, "~", uts.release,
		    NULL, &error) < 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't write boot entry\n");

      exit (EXIT_FAILURE);
    }

  return EXIT_SUCCESS;
}

static int
main_boottime (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {"file", required_argument, NULL, 'f'},
    {NULL, 0, NULL, '\0'}
  };
  char *error = NULL;
  int c;
  uint64_t boottime;

  while ((c = getopt_long (argc, argv, "f:hv", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'f':
          wtmpdb_path = optarg;
          break;
        case 'v':
          show_version();
          break;
        case 'h':
          usage (EXIT_SUCCESS, CMD_BOOTTIME);
          break;
        default:
          usage (EXIT_FAILURE, CMD_BOOTTIME);
          break;
        }
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE, CMD_BOOTTIME);
    }

  boottime = wtmpdb_get_boottime (wtmpdb_path, &error);
  if (error)
    {
      fprintf (stderr, "Couldn't read boot entry: %s\n", error);
      free (error);
      exit (EXIT_FAILURE);
    }

  char timebuf[32];
  format_time (TIMEFMT_CTIME, timebuf, sizeof(timebuf), boottime);

  printf ("system boot %s\n", timebuf);

  return EXIT_SUCCESS;
}

static int
main_shutdown (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {"file", required_argument, NULL, 'f'},
    {NULL, 0, NULL, '\0'}
  };
  char *error = NULL;
  int c;

  while ((c = getopt_long (argc, argv, "f:hv", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'f':
          wtmpdb_path = optarg;
          break;
        case 'v':
          show_version();
          break;
        case 'h':
          usage (EXIT_SUCCESS, CMD_SHUTDOWN);
          break;
        default:
          usage (EXIT_FAILURE, CMD_SHUTDOWN);
          break;
        }
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE, CMD_SHUTDOWN);
    }

#if HAVE_AUDIT
  log_audit (AUDIT_SYSTEM_SHUTDOWN);
#endif

  int64_t id = wtmpdb_get_id (wtmpdb_path, "~", &error);
  if (id < 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't get ID for reboot entry\n");

      exit (EXIT_FAILURE);
    }

  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);
  uint64_t time = wtmpdb_timespec2usec (ts);

  if (wtmpdb_logout (wtmpdb_path, id, time, &error) < 0)
    {
      if (error)
        {
          fprintf (stderr, "%s\n", error);
          free (error);
        }
      else
        fprintf (stderr, "Couldn't write shutdown entry\n");

      exit (EXIT_FAILURE);
    }

  return EXIT_SUCCESS;
}

static int
main_import (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {"file", required_argument, NULL, 'f'},
    {NULL, 0, NULL, '\0'}
  };
  int c;

  while ((c = getopt_long (argc, argv, "f:hv", longopts, NULL)) != -1)
    {
      switch (c)
        {
        case 'f':
          wtmpdb_path = optarg;
          break;
        case 'v':
          show_version();
          break;
        case 'h':
          usage (EXIT_SUCCESS, CMD_IMPORT);
          break;
        default:
          usage (EXIT_FAILURE, CMD_IMPORT);
          break;
        }
    }

  if (argc == optind)
    {
      fprintf (stderr, "No files specified to import.\n");
      usage (EXIT_FAILURE, CMD_IMPORT);
    }

  for (; optind < argc; optind++)
    if (import_wtmp_file (wtmpdb_path, argv[optind]) == -1)
      return EXIT_FAILURE;

  return EXIT_SUCCESS;
}

int
main (int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {"file", required_argument,     NULL, 'f'},
    {NULL, 0, NULL, '\0'}
  };
  int c;

  if (strcmp (basename(argv[0]), "last") == 0) {
	legacy = 1;
    return main_last (argc, argv);
  }
  if (strcmp (basename(argv[0]), "wlast") == 0)
    return main_last (argc, argv);
  if (strcmp (basename(argv[0]), "lastlog") == 0) {
	legacy = 1;
	uniq = 1;
    return main_last (argc, argv);
  }
  if (strcmp (basename(argv[0]), "wlastlog") == 0) {
	uniq = 1;
    return main_last (argc, argv);
  }
  if (argc == 1)
    usage (EXIT_SUCCESS, CMD_NONE);
  if (strcmp (argv[1], cmd_name[CMD_LAST]) == 0)
    return main_last (--argc, ++argv);
  if (strcmp (argv[1], cmd_name[CMD_BOOT]) == 0)
    return main_boot (--argc, ++argv);
  if (strcmp (argv[1], cmd_name[CMD_SHUTDOWN]) == 0)
    return main_shutdown (--argc, ++argv);
  if (strcmp (argv[1], cmd_name[CMD_BOOTTIME]) == 0)
    return main_boottime (--argc, ++argv);
  if (strcmp (argv[1], cmd_name[CMD_ROTATE]) == 0)
    return main_rotate (--argc, ++argv);
  if (strcmp (argv[1], cmd_name[CMD_IMPORT]) == 0)
    return main_import (--argc, ++argv);

  while ((c = getopt_long (argc, argv, "f:hv", longopts, NULL)) != -1)
    {
      switch (c)
	{
	case 'h':
	  usage (EXIT_SUCCESS, CMD_NONE);
	  break;
	case 'v':
	  show_version();
	  break;
	case 'f':
	  /* ignore common option */
	  break;
	default:
	  usage (EXIT_FAILURE, CMD_NONE);
	  break;
	}
    }

  if (argc > optind)
    {
      fprintf (stderr, "Unexpected argument: %s\n", argv[optind]);
      usage (EXIT_FAILURE, CMD_NONE);
    }

  exit (EXIT_SUCCESS);
}
