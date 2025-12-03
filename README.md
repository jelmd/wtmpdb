
# wtmpdb

This is a fork of [wtmdb](https://github.com/thkukuk/wtmpdb), a Year-2038-safe database for logins, logouts, machine boots, and shutdowns.

By default, the wtmpdb tools and PAM module use the SQLite database file `/var/lib/wtmpdb/wtmp.db`.

Because most tools that allow users to log in rely on the Pluggable Authentication Module (PAM) framework, the idea is to add the PAM module **pam_wtmpdb.so** to the PAM session stack to reliably record this information. This frees applications from having to write wtmp records themselves; the apps do not need to be rewritten or adjusted. They may continue writing to the legacy sparse file `/var/log/wtmp` if they wish and can phase that out later to get rid of this burden.

To record machine boots and shutdowns properly, a service or rc.local script should invoke `wtmpdb boot` or `wtmpdb shutdown` at those events, because PAM is typically not involved in these system transitions.


## NOTES:
When OpenSSH (and possibly other tools) authenticates a user via PAM, the TTY (if any) may not yet be known. In this case, **wtmpdb** records the PAM service name instead of the actual TTY used by the process.
If the TTY is really needed, it can be looked up using `w` or `who` while the user is still logged in.

If some tools eventually begin modifying the wtmp DB used by the PAM module (e.g. by calling the login/logout functions provided by the wtmpdb library), you may either skip recording it for such tools via the pam module parameter **skip_if=...**, or instruct the PAM module to use a separate database via **database=...**. If in doubt, use the pam module parameter **debug** to inspect its behavior.


## Fork Enhancements
Compared to the original, this fork adds features that better reflect modern workflows rather than the conventions of 50+ years ago:
- A far less cluttered and more consolidated help and manpage - see [PR#45](https://github.com/thkukuk/wtmpdb/pull/45).
- Convenience (symlink) name **wlast**: for users who don’t want to type "wtmpdb last ..." repeatedly, and for more precise session duration (seconds instead of minutes) - see [PR#46](https://github.com/thkukuk/wtmpdb/pull/46).
- Convenience (symlink) names **lastlog** and **wlastlog**: show the most recent login entry for each user since the last boot, only; the **wlastlog** variant also shows precise session duration - see [PR#48](https://github.com/thkukuk/wtmpdb/pull/48).
- Option `--open` (`-o`): show only sessions that are still open since the last boot - see [PR#52](https://github.com/thkukuk/wtmpdb/pull/52).
- The _time-format_ **compact**, which formats timestamps as `YYYY-mm-dd HH:MM:SS` - see [PR#54](https://github.com/thkukuk/wtmpdb/pull/54/commits/9f7ac3cd98c16006ce4621a3888c75e5c55da733).
- Option `--compact` (`-c`): hide the redundant logout timestamp column and set the default login timestamp format to `compact`. If the **LAST_COMPACT** environment variable is set, this mode is enabled automatically - see [PR#54](https://github.com/jelmd/wtmpdb/commit/9f7ac3cd98c16006ce4621a3888c75e5c55da733).

## Configuration
Add the following line to the _session_ section of your system’s PAM stack.
```
session optional pam_wtmpdb.so
```
E.g. a good place to insert this is near the end of one of the following files:
- openSUSE Tumbleweed / MicroOSe: `/etc/pam.d/postlogin-session`
- Ubuntu: `/etc/pam.d/common-session`.


### Daemon
The included **wtmpdbd** daemon provides a systemd Varlink interface to record logins and logouts without needing direct write access to the database file. Since this allows arbitrary tools to modify (or potentially trash) the wtmp DB, it should not be enabled unless absolutely necessary.

As noted earlier, tools that allow access to the system should authenticate via PAM, and with **pam_wtmpdb.so** properly integrated, there is no need for any additional tool to modify the database directly (at least not in a way provided by the wtmpdb library or varlink interface right now).


### Special Requirements
To build **wtmpdbd**, systemd >= v257 is required. If you only want to build **wtmpdb** and **pam_wtmpdb.so**, it is recommended to disable systemd usage. However, if you wish for `wtmpdb boot` to differentiate between a soft and hard reboot (queries the systemd manager for SoftRebootsCount) and record it accordingly, you should keep systemd enabled. Without systemd, a boot gets simply recorded as `reboot`.

### Build
To build the binaries, you may run e.g.:
```
meson configure -Dsystemd=disabled --buildtype=release --prefix=/usr build/
ninja -v -C build
```

### Example Output
The following examples show the output of various `wtmpdb last` runs using its **compact mode** (**-c**) option:
```
> build/wtmpdb last -c
hans     tty7         :0               2025-12-03 17:37:44 .(05:29:46)
wurst    tty7         :0               2025-12-03 17:37:27 .(05:30:03)
hans     tty7         :0               2025-12-03 14:26:30  (03:10:56)
karl     ssh          123.45.67.89     2025-12-03 10:27:02  (00:06:11)
ranseier ssh          123.45.67.890    2025-12-03 10:23:33 .(12:43:57)
alexa    ssh          123.45.67.89     2025-12-03 08:49:25  (04:00:25)
hans     ssh          12.345.678.90    2025-12-03 07:06:32 .(16:00:58)
hans     ssh          12.345.678.90    2025-12-03 07:02:34  (00:01:41)
wurst    tty7         :0               2025-12-02 20:18:49 .(1+02:48:41)
hans     tty7         :0               2025-12-02 14:59:14  (05:20:04)
hans     ssh          12.345.678.90    2025-12-02 13:53:10  (00:58:09)
platz    ssh          123.45.678.90    2025-12-02 10:37:36  (00:58:26)
alexa    ssh          123.45.67.89     2025-12-02 10:27:10  (00:00:59)
alexa    ssh          123.45.67.89     2025-12-02 09:29:36  (00:00:46)
alexa    ssh          123.456.789.01   2025-12-02 08:39:10  (00:11:53)
alexa    ssh          123.456.789.01   2025-12-02 07:51:33  (00:55:11)
hans     ssh          12.345.678.90    2025-12-02 00:49:31  (14:01:43)
wurst    tty7         :0               2025-12-01 22:13:02 .(2+00:54:28)
hans     tty7         :0               2025-12-01 15:00:24  (07:12:37)
...
reboot   system boot  6.8.0-84-generic 2025-11-26 20:53:04 .(7+02:14:26)
andre    console      2025-11-26       2025-11-26 20:41:41 ?(00:11:22)
platz    ssh          12.345.67.8      2025-11-26 18:51:49  (00:05:05)
hans     tty7         :0               2025-11-26 18:45:05  (02:04:00)
hans     ssh          901.23.45.67     2025-11-26 18:44:45  (02:07:21)
andre    ssh          901.23.45.67     2025-11-26 18:44:18  (02:07:48)
wurst    tty7         :0               2025-11-26 18:42:48  (00:02:17)
andre    tty7         :0               2025-11-26 18:42:47 ?(02:10:17)
wurst    tty7         :0               2025-11-26 18:42:26 ?(02:10:37)
reboot   system boot  6.8.0-84-generic 2025-11-26 18:42:17  (02:06:49)
andre    ssh          890.12.34.5      2025-11-26 18:40:36 ?(00:01:40)
andre    ssh          678.90.12.345    2025-11-26 17:44:31 ?(00:57:45)
hans     tty7         :0               2025-11-26 13:51:44 ?(04:50:32)
hans     ssh          12.345.678.90    2025-11-26 12:51:04 ?(05:51:12)
alexa    ssh          123.45.67.89     2025-11-26 10:14:22  (03:47:25)
alexa    ssh          123.45.67.89     2025-11-26 10:12:39  (00:00:19)
ranseier ssh          123.45.67.890    2025-11-26 09:21:10 ?(09:21:06)
platz    ssh          678.90.12.34     2025-11-26 09:06:33  (07:36:46)
hans     ssh          12.345.678.90    2025-11-26 08:00:20 ?(10:41:56)
hans     ssh          12.345.678.90    2025-11-26 01:41:51  (11:47:04)

wtmpdb begins 2025-11-26 01:41:51


# show sessions without a logout entry (-o):
> build/wtmpdb last -co

hans     tty7         :0               2025-12-03 17:37:44 .(05:30:20)
wurst    tty7         :0               2025-12-03 17:37:27 .(05:30:37)
karl     ssh          123.45.67.890    2025-12-03 10:23:33 .(12:44:31)
hans     ssh          12.345.678.90    2025-12-03 07:06:32 .(16:01:32)
wurst    tty7         :0               2025-12-02 20:18:49 .(1+02:49:15)
wurst    tty7         :0               2025-12-01 22:13:02 .(2+00:55:02)
...
reboot   system boot  6.8.0-84-generic 2025-11-26 20:53:04 .(7+02:15:00)
ranse    console      2025-11-26       2025-11-26 20:41:41 ?(00:11:22)
ranse    tty7         :0               2025-11-26 18:42:47 ?(02:10:17)
wurst    tty7         :0               2025-11-26 18:42:26 ?(02:10:37)
ranse    ssh          123.45.67.8      2025-11-26 18:40:36 ?(00:01:40)
ranse    ssh          901.23.45.678    2025-11-26 17:44:31 ?(00:57:45)
hans     tty7         :0               2025-11-26 13:51:44 ?(04:50:32)
hans     ssh          12.345.678.90    2025-11-26 12:51:04 ?(05:51:12)
karl     ssh          123.45.67.890    2025-11-26 09:21:10 ?(09:21:06)
hans     ssh          12.345.678.90    2025-11-26 08:00:20 ?(10:41:56)
...
wtmpdb begins 2025-11-26 01:41:51


# show sessions still running (-p now):
> build/wtmpdb last -cp now
hans     tty7         :0               2025-12-03 17:37:44 .(05:30:05)
wurst    tty7         :0               2025-12-03 17:37:27 .(05:30:22)
karl     ssh          123.45.67.890    2025-12-03 10:23:33 .(12:44:16)
hans     ssh          12.345.678.90    2025-12-03 07:06:32 .(16:01:17)
wurst    tty7         :0               2025-12-02 20:18:49 .(1+02:49:00)
wurst    tty7         :0               2025-12-01 22:13:02 .(2+00:54:47)
...
reboot   system boot  6.8.0-84-generic 2025-11-26 20:53:04 .(7+02:14:45)

wtmpdb begins 2025-09-16 03:04:00


# show the latest session for each user (-u):
> build/wtmpdb last -cu
hans     console      2025-11-26       2025-11-26 20:41:41 .(7+02:26:33)
wurst    tty7         :0               2025-12-03 17:37:44 .(05:30:30)
karl     tty7         :0               2025-12-03 17:37:27 .(05:30:47)
ranseier ssh          123.45.678.90    2025-12-02 10:37:36  (00:58:26)
alexande ssh          123.45.67.890    2025-12-03 10:23:33 .(12:44:41)
platz    ssh          123.45.67.89     2025-12-03 08:49:25  (04:00:25)
andreas  ssh          123.45.67.89     2025-12-03 10:27:02  (00:06:11)

wtmpdb begins 2025-11-26 20:41:41
```
