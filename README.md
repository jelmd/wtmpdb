
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
