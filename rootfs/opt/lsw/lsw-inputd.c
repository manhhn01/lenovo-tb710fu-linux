// lsw-inputd: one small daemon for everything the tablet's KDE session needs from the input side.
// Runs as root inside the container during a session. It replaces udevd for the devices we allow:
//   - input devices, displays, USB drives and USB sound: creates nodes and udev entries and
//     re-sends kernel events in libudev's format so KWin, UDisks and PipeWire see them.
//     The tablet's own flash (sda..sdf) and sound card (card0) are never touched.
//   - pen charging notice, from the charger driver's line in the kernel log. Only reads the log:
//     touching the charger chip's sysfs files may have rebooted the tablet once.
//   - pen gestures over Bluetooth: double tap = right click, swipe up = Ctrl+Z,
//     swipe down = Ctrl+Shift+Z, sent from a virtual input device.
//   - keyboard case: hall_irq sends key 0xfc on close (lock) and 0xfd on open (wake display).
//   - battery state: Android's kernel reports charging current as negative, so UPower shows
//     "discharging" while charging. A bind mount over current_now presents it with the sign flipped.
//   - battery log: one line a minute in /var/lib/lsw/battery-log.csv, to measure battery life.
//   - tablet mode: on when the keyboard cover is detached. KWin's automatic check cannot work here
//     because the virtual gesture device counts as a mouse.
//
// Usage: lsw-inputd                  run (one instance per session)
//        lsw-inputd --init           write the udev database for the devices present now
//        lsw-inputd --classify       print what each input device is taken for, change nothing
//        lsw-inputd --replay eventN  announce existing input devices, then exit
//        lsw-inputd --hash text ...  print libudev's filter hash (for testing)
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#define DESKTOP_UID 1000
#define DESKTOP_GID 1000
#define DISK_GID 6
#define GROUP_KERNEL 1
#define GROUP_UDEV 2
#define MAX_PAIRS 96
#define MAX_EVENTS 128
#define PEN_SILENCE_MS 6000
#define KMSG_INTERVAL_MS 200
#define KMSG_BURST 300

#define HALL_CLOSE 0xfc
#define HALL_OPEN 0xfd

// The pen's gestures arrive as HID codes with no key name; give each a real key first.
static const struct { uint32_t scancode, key; } pen_keys[] = {
    { 0xc0601, KEY_PROG1 }, // double tap
    { 0xc0613, KEY_PROG2 }, // swipe up
    { 0xc0612, KEY_PROG3 }, // swipe down
};
static const struct { unsigned key; int n; int out[3]; } actions[] = {
    { KEY_PROG1, 1, { BTN_RIGHT } },
    { KEY_PROG2, 2, { KEY_LEFTCTRL, KEY_Z } },
    { KEY_PROG3, 3, { KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_Z } },
};

static void say(const char *fmt, ...) {
    char when[16];
    time_t now = time(NULL);
    strftime(when, sizeof when, "%H:%M:%S", localtime(&now));
    printf("%s ", when);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_text(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0)
        return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        n--;
    buf[n] = 0;
    return 0;
}

static void drop(const char *path) {
    if (unlink(path) < 0 && errno != ENOENT)
        say("cannot remove %s: %s", path, strerror(errno));
}

static void make_dirs(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    mkdir(tmp, 0755);
}

// ---- libudev message format ----
static uint32_t murmur2(const char *text) {
    const uint32_t m = 0x5bd1e995;
    size_t len = strlen(text);
    const unsigned char *d = (const unsigned char *)text;
    uint32_t h = (uint32_t)len;
    while (len >= 4) {
        uint32_t k = d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24;
        k *= m;
        k ^= k >> 24;
        k *= m;
        h = h * m ^ k;
        d += 4;
        len -= 4;
    }
    if (len == 3)
        h ^= (uint32_t)d[2] << 16;
    if (len >= 2)
        h ^= (uint32_t)d[1] << 8;
    if (len >= 1) {
        h ^= d[0];
        h *= m;
    }
    h ^= h >> 13;
    h *= m;
    return h ^ h >> 15;
}

struct pairs {
    char *item[MAX_PAIRS];
    int n;
};

static void put(struct pairs *p, const char *fmt, ...) {
    if (p->n >= MAX_PAIRS)
        return;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(&p->item[p->n], fmt, ap) >= 0)
        p->n++;
    va_end(ap);
}

static void clear_pairs(struct pairs *p) {
    for (int i = 0; i < p->n; i++)
        free(p->item[i]);
    p->n = 0;
}

static int udev_tx = -1;
static unsigned long long sequence = 1ULL << 40;

static void send_event(struct pairs *p, const char *subsystem, const char *devtype) {
    char body[8192];
    size_t len = 0;
    put(p, "SEQNUM=%llu", ++sequence);
    put(p, "USEC_INITIALIZED=1");
    for (int i = 0; i < p->n; i++) {
        size_t l = strlen(p->item[i]) + 1;
        if (len + l > sizeof body)
            break;
        memcpy(body + len, p->item[i], l);
        len += l;
    }
    unsigned char head[40] = "libudev";
    uint32_t v = htonl(0xfeedcafe);
    memcpy(head + 8, &v, 4);
    v = 40;
    memcpy(head + 12, &v, 4);                 // header size
    memcpy(head + 16, &v, 4);                 // properties offset
    v = (uint32_t)len;
    memcpy(head + 20, &v, 4);                 // properties length
    v = htonl(murmur2(subsystem));
    memcpy(head + 24, &v, 4);
    v = htonl(devtype ? murmur2(devtype) : 0);
    memcpy(head + 28, &v, 4);
    struct sockaddr_nl to = { .nl_family = AF_NETLINK, .nl_groups = GROUP_UDEV };
    struct iovec iov[2] = { { head, sizeof head }, { body, len } };
    struct msghdr msg = { .msg_name = &to, .msg_namelen = sizeof to, .msg_iov = iov, .msg_iovlen = 2 };
    if (sendmsg(udev_tx, &msg, 0) < 0 && errno != ECONNREFUSED && errno != ESRCH)
        say("send failed: %s", strerror(errno));
}

// ---- input device classification ----
#define BIT_WORDS 12
struct bits {
    uint64_t w[BIT_WORDS];
};

static int has(const struct bits *b, unsigned n) {
    return n / 64 < BIT_WORDS && (b->w[n / 64] >> (n % 64)) & 1;
}

static int any_in(const struct bits *b, unsigned lo, unsigned hi) {
    for (unsigned i = lo; i <= hi; i++)
        if (has(b, i))
            return 1;
    return 0;
}

static void read_bits(const char *ev, const char *file, struct bits *b) {
    char path[160], text[1024];
    memset(b, 0, sizeof *b);
    snprintf(path, sizeof path, "/sys/class/input/%s/device/%s", ev, file);
    if (read_text(path, text, sizeof text) < 0)
        return;
    char *word[64];
    int n = 0;
    for (char *t = strtok(text, " "); t && n < 64; t = strtok(NULL, " "))
        word[n++] = t;
    for (int i = 0; i < n && i < BIT_WORDS; i++)
        b->w[i] = strtoull(word[n - 1 - i], NULL, 16);
}

static int device_name(const char *ev, char *name, size_t size) {
    char path[160];
    snprintf(path, sizeof path, "/sys/class/input/%s/device/name", ev);
    return read_text(path, name, size);
}

struct props {
    const char *key[4];
    const char *value[4];
    int n;
};

static void prop(struct props *p, const char *key, const char *value) {
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->key[i], key))
            return;
    p->key[p->n] = key;
    p->value[p->n++] = value;
}

static void classify_caps(const struct bits *key, const struct bits *abs, const struct bits *rel,
                          const struct bits *pr, struct props *p) {
    const unsigned BTN_L = BTN_LEFT, PEN = BTN_TOOL_PEN, FINGER = BTN_TOOL_FINGER, TOUCH = BTN_TOUCH;
    int direct = has(pr, INPUT_PROP_DIRECT);
    int joystick = any_in(key, 0x120, 0x13f) || any_in(key, 0x2c0, 0x2e7);
    if (has(abs, ABS_X) && has(abs, ABS_Y)) {
        if (has(key, PEN))
            prop(p, "ID_INPUT_TABLET", "1");
        else if (direct || (has(key, TOUCH) && !has(key, FINGER)))
            prop(p, "ID_INPUT_TOUCHSCREEN", "1");
        else if (has(key, FINGER)) {
            prop(p, "ID_INPUT_TOUCHPAD", "1");
            prop(p, "ID_INPUT_MOUSE", "1");
        } else if (joystick)
            prop(p, "ID_INPUT_JOYSTICK", "1");
        else if (has(key, BTN_L))
            prop(p, "ID_INPUT_MOUSE", "1");
    } else if (direct && has(abs, ABS_MT_POSITION_X) && has(abs, ABS_MT_POSITION_Y))
        prop(p, "ID_INPUT_TOUCHSCREEN", "1");
    if (has(rel, REL_X) && has(rel, REL_Y) && has(key, BTN_L))
        prop(p, "ID_INPUT_MOUSE", "1");
    if ((key->w[0] & 0xfffffffeULL) == 0xfffffffeULL) {
        prop(p, "ID_INPUT_KEYBOARD", "1");
        prop(p, "ID_INPUT_KEY", "1");
    } else if (any_in(key, 1, 0x10f) || any_in(key, 0x160, 0x2bf))
        prop(p, "ID_INPUT_KEY", "1");
}

// Returns 0 and fills p, or -1 if the device must stay away from the desktop.
static int classify(const char *ev, char *name, size_t size, struct props *p) {
    static const char *matrix = "0 1 0 -1 0 1";    // the panel's touch and pen report in portrait
    memset(p, 0, sizeof *p);
    if (device_name(ev, name, size) < 0)
        return -1;
    if (strstr(name, "qcom-hv-haptics") || strstr(name, "snd-card"))
        return -1;
    if (!strcmp(name, "hall_irq")) {
        prop(p, "ID_INPUT_SWITCH", "1");
        return 0;
    }
    if (strstr(name, "NVTCapacitiveTouchScreen")) {
        prop(p, "ID_INPUT_TOUCHSCREEN", "1");
        prop(p, "LIBINPUT_CALIBRATION_MATRIX", matrix);
        return 0;
    }
    if (strstr(name, "NVTCapacitivePen")) {
        prop(p, "ID_INPUT_TABLET", "1");
        prop(p, "LIBINPUT_CALIBRATION_MATRIX", matrix);
        return 0;
    }
    struct bits key, abs, rel, pr;
    read_bits(ev, "capabilities/key", &key);
    read_bits(ev, "capabilities/abs", &abs);
    read_bits(ev, "capabilities/rel", &rel);
    read_bits(ev, "properties", &pr);
    classify_caps(&key, &abs, &rel, &pr, p);
    return p->n ? 0 : -1;
}

static void print_props(const struct props *p) {
    printf("{");
    for (int i = 0; i < p->n; i++)
        printf("%s'%s': '%s'", i ? ", " : "", p->key[i], p->value[i]);
    printf("}");
}

static int major_minor(const char *ev, unsigned *maj, unsigned *min) {
    char path[160], text[32];
    snprintf(path, sizeof path, "/sys/class/input/%s/dev", ev);
    return read_text(path, text, sizeof text) < 0 || sscanf(text, "%u:%u", maj, min) != 2 ? -1 : 0;
}

static void write_input_db(unsigned maj, unsigned min, const struct props *p) {
    char path[64];
    make_dirs("/run/udev/data");
    snprintf(path, sizeof path, "/run/udev/data/c%u:%u", maj, min);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fputs("I:1\nE:ID_INPUT=1\n", f);
    for (int i = 0; i < p->n; i++)
        fprintf(f, "E:%s=%s\n", p->key[i], p->value[i]);
    fputs("E:ID_SEAT=seat0\nG:seat\nQ:seat\n", f);
    fclose(f);
}

static int event_filter(const struct dirent *d) {
    return !strncmp(d->d_name, "event", 5);
}

static int list_events(struct dirent ***out) {
    return scandir("/sys/class/input", out, event_filter, alphasort);
}

// ---- running desktop programs for the user ----
static int find_session_bus(char *out, size_t size) {
    DIR *d = opendir("/proc");
    if (!d)
        return -1;
    int found = -1;
    struct dirent *e;
    while (found < 0 && (e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char path[64], comm[32];
        snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
        if (read_text(path, comm, sizeof comm) < 0 || strcmp(comm, "plasmashell"))
            continue;
        snprintf(path, sizeof path, "/proc/%s/environ", e->d_name);
        static char env[65536];
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        ssize_t n = read(fd, env, sizeof env - 1);
        close(fd);
        for (ssize_t i = 0; i < n;) {
            size_t l = strnlen(env + i, (size_t)(n - i));
            if (!strncmp(env + i, "DBUS_SESSION_BUS_ADDRESS=", 25)) {
                snprintf(out, size, "%.*s", (int)l, env + i);
                found = 0;
                break;
            }
            i += (ssize_t)l + 1;
        }
    }
    closedir(d);
    return found;
}

static void run_as_user(const char *const cmd[]) {
    char bus[512];
    if (find_session_bus(bus, sizeof bus) < 0)
        return;
    const char *args[32] = { "setpriv", "--reuid=1000", "--regid=1000", "--init-groups", "env", "-i",
                             "PATH=/usr/bin:/bin", "HOME=/home/user", "XDG_RUNTIME_DIR=/run/user/1000",
                             "WAYLAND_DISPLAY=wayland-0", bus };
    int n = 11;
    for (int i = 0; cmd[i] && n < 31; i++)
        args[n++] = cmd[i];
    args[n] = NULL;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        execv("/usr/bin/setpriv", (char *const *)args);
        _exit(127);
    }
}

static void notify_pen(const char *title, const char *body) {
    const char *cmd[] = { "notify-send", "-a", "Stylus", "-i", "input-tablet", "-t", "5000", title, body, NULL };
    run_as_user(cmd);
}

// ---- pen gestures ----
static int uinput_fd = -1, gesture_fd = -1;

static void open_uinput(void) {
    if (access("/dev/uinput", F_OK) < 0) {
        char text[32];
        unsigned maj, min;
        if (read_text("/sys/class/misc/uinput/dev", text, sizeof text) == 0 && sscanf(text, "%u:%u", &maj, &min) == 2)
            mknod("/dev/uinput", S_IFCHR | 0600, makedev(maj, min));
    }
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        say("cannot open /dev/uinput: %s", strerror(errno));
        return;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);                // REL_X, REL_Y: so it is taken for a pointer
    ioctl(fd, UI_SET_RELBIT, REL_Y);
    static const int keys[] = { BTN_LEFT, BTN_RIGHT, KEY_LEFTCTRL, KEY_LEFTSHIFT, KEY_Z };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        ioctl(fd, UI_SET_KEYBIT, keys[i]);
    struct uinput_user_dev dev;
    memset(&dev, 0, sizeof dev);
    snprintf(dev.name, sizeof dev.name, "LSW pen gestures");
    dev.id.bustype = BUS_VIRTUAL;
    dev.id.vendor = 0x17ef;
    dev.id.product = 0x61a1;
    dev.id.version = 1;
    if (write(fd, &dev, sizeof dev) != (ssize_t)sizeof dev || ioctl(fd, UI_DEV_CREATE) < 0) {
        say("cannot create the virtual device: %s", strerror(errno));
        close(fd);
        return;
    }
    uinput_fd = fd;
    say("virtual device ready");
}

static void emit(int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned short)type;
    ev.code = (unsigned short)code;
    ev.value = value;
    if (write(uinput_fd, &ev, sizeof ev) < 0)
        say("uinput write failed: %s", strerror(errno));
}

static void press(const int *keys, int n) {
    for (int i = 0; i < n; i++)
        emit(EV_KEY, keys[i], 1);
    emit(EV_SYN, SYN_REPORT, 0);
    usleep(20000);
    for (int i = n - 1; i >= 0; i--)
        emit(EV_KEY, keys[i], 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

static int is_pen_gestures(const char *name) {
    return strstr(name, "Lenovo Tab Pen") && strstr(name, "Consumer Control");
}

static void apply_pen_keys(const char *node) {
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return;
    for (size_t i = 0; i < sizeof pen_keys / sizeof *pen_keys; i++) {
        unsigned int map[2] = { pen_keys[i].scancode, pen_keys[i].key };
        if (ioctl(fd, EVIOCSKEYCODE, map) < 0)
            say("keymap failed for %#x: %s", map[0], strerror(errno));
    }
    close(fd);
    say("pen gesture keys set on %s", node);
}

static void take_gestures(const char *node) {
    if (gesture_fd >= 0)
        close(gesture_fd);
    gesture_fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (gesture_fd < 0)
        return;
    if (ioctl(gesture_fd, EVIOCGRAB, 1) < 0)
        say("cannot grab %s: %s", node, strerror(errno));
    say("pen gestures from %s", node);
}

static void handle_gestures(void) {
    struct input_event ev[32];
    ssize_t n = read(gesture_fd, ev, sizeof ev);
    if (n < 0) {
        if (errno == EAGAIN)
            return;
        say("pen lost: %s", strerror(errno));
        close(gesture_fd);
        gesture_fd = -1;
        return;
    }
    for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
        if (ev[i].type != EV_KEY || ev[i].value != 1)
            continue;
        for (size_t a = 0; a < sizeof actions / sizeof *actions; a++)
            if (actions[a].key == ev[i].code && uinput_fd >= 0) {
                say("gesture %u", ev[i].code);
                press(actions[a].out, actions[a].n);
            }
    }
}

// ---- keyboard case ----
static int hall_fd = -1;

static void handle_hall(void) {
    struct input_event ev[16];
    ssize_t n = read(hall_fd, ev, sizeof ev);
    if (n < 0)
        return;
    for (size_t i = 0; i < (size_t)n / sizeof *ev; i++) {
        if (ev[i].type != EV_KEY || ev[i].value != 1)
            continue;
        if (ev[i].code == HALL_CLOSE) {
            say("case closed");
            const char *cmd[] = { "busctl", "--user", "call", "org.freedesktop.ScreenSaver", "/ScreenSaver",
                                  "org.freedesktop.ScreenSaver", "Lock", NULL };
            run_as_user(cmd);
        } else if (ev[i].code == HALL_OPEN) {
            say("case opened");
            const char *cmd[] = { "kscreen-doctor", "--dpms", "on", NULL };
            run_as_user(cmd);
        }
    }
}

// ---- pen charging notice ----
static int kmsg_fd = -1, pen_attached;
static int64_t pen_last_seen;
static char pen_level[16] = "?";

static void open_kmsg(void) {
    if (access("/dev/kmsg", F_OK) < 0)
        mknod("/dev/kmsg", S_IFCHR | 0640, makedev(1, 11));
    kmsg_fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (kmsg_fd < 0)
        say("cannot open /dev/kmsg: %s", strerror(errno));
    else
        lseek(kmsg_fd, 0, SEEK_END);
}

static void pen_line(const char *line) {
    const char *l = strstr(line, "pen_info_uevent");
    if (!l)
        return;
    const char *lv = strstr(l, "LEVEL="), *at = strstr(l, "ATTACHED=");
    if (!lv || !at || !isdigit((unsigned char)lv[6]) || !isdigit((unsigned char)at[9]))
        return;
    char level[16];
    snprintf(level, sizeof level, "%d", atoi(lv + 6));
    char title[48], body[48];
    if (at[9] == '1') {
        if (!pen_attached) {
            say("pen attached %s", level);
            snprintf(title, sizeof title, "Pen charging: %s%%", level);
            notify_pen(title, "");
        }
        pen_attached = 1;
        pen_last_seen = now_ms();
        snprintf(pen_level, sizeof pen_level, "%s", level);
    } else if (pen_attached) {
        pen_attached = 0;
        say("pen removed (reported)");
        snprintf(body, sizeof body, "Battery: %s%%", level);
        notify_pen("Pen removed", body);
    }
}

static void drain_kmsg(void) {
    char line[2048];
    for (int i = 0; i < KMSG_BURST; i++) {
        ssize_t n = read(kmsg_fd, line, sizeof line - 1);
        if (n < 0) {
            if (errno == EPIPE)                     // the log overran our position: carry on
                continue;
            break;
        }
        line[n] = 0;
        pen_line(line);
    }
}

static void check_pen_silence(void) {
    if (pen_attached && now_ms() - pen_last_seen > PEN_SILENCE_MS) {
        char body[48];
        pen_attached = 0;
        say("pen removed (silence)");
        snprintf(body, sizeof body, "Battery: %s%%", pen_level);
        notify_pen("Pen removed", body);
    }
}

// ---- battery current sign ----
#define BAT_REAL "/run/droidspaces/sys/class/power_supply/battery/current_now"
#define BAT_SHOWN "/run/lsw-battery-current"
static int battery_ready;
static int64_t battery_next;

static void battery_tick(void) {
    int64_t now = now_ms();
    if (now < battery_next)
        return;
    battery_next = now + 5000;
    char text[32], target[4096];
    if (read_text(BAT_REAL, text, sizeof text) < 0)
        return;
    int fd = open(BAT_SHOWN, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    char out[32];
    int n = snprintf(out, sizeof out, "%lld\n", -atoll(text));
    if (ftruncate(fd, 0) == 0 && pwrite(fd, out, (size_t)n, 0) < 0)
        say("battery write failed: %s", strerror(errno));
    close(fd);
    if (battery_ready)
        return;
    battery_ready = 1;
    if (!realpath("/sys/class/power_supply/battery/current_now", target))
        return;
    FILE *m = fopen("/proc/self/mountinfo", "r");
    char line[512];
    int mounted = 0;
    while (m && fgets(line, sizeof line, m))
        if (strstr(line, target))
            mounted = 1;
    if (m)
        fclose(m);
    if (!mounted && mount(BAT_SHOWN, target, NULL, MS_BIND, NULL) < 0)
        say("battery bind failed: %s", strerror(errno));
    else
        say("battery current sign flipped for UPower");
}

// ---- battery log ----
#define BATTERY_LOG "/var/lib/lsw/battery-log.csv"
static int64_t log_next;

static long read_num(const char *path) {
    char text[32];
    return read_text(path, text, sizeof text) < 0 ? 0 : atol(text);
}

static void battery_log(void) {
    int64_t now = now_ms();
    if (now < log_next)
        return;
    log_next = now + 60000;
    struct stat st;
    int fresh = stat(BATTERY_LOG, &st) < 0 || st.st_size > 2000000;   // about a month of lines
    FILE *f = fopen(BATTERY_LOG, fresh ? "w" : "a");
    if (!f)
        return;
    if (fresh)
        fputs("time,percent,status,current_mA,voltage_mV,temp_C,brightness,display\n", f);
    const char *B = "/run/droidspaces/sys/class/power_supply/battery";
    char path[128], status[24] = "?", display[12] = "?", when[24];
    snprintf(path, sizeof path, "%s/status", B);
    read_text(path, status, sizeof status);
    read_text("/sys/class/drm/card0-DSI-1/dpms", display, sizeof display);
    time_t t = time(NULL);
    strftime(when, sizeof when, "%F %T", localtime(&t));
    snprintf(path, sizeof path, "%s/capacity", B);
    long percent = read_num(path);
    snprintf(path, sizeof path, "%s/current_now", B);
    long current = read_num(path) / 1000;
    snprintf(path, sizeof path, "%s/voltage_now", B);
    long voltage = read_num(path) / 1000;
    snprintf(path, sizeof path, "%s/temp", B);
    long temp = read_num(path) / 10;
    fprintf(f, "%s,%ld,%s,%ld,%ld,%ld,%ld,%s\n", when, percent, status, current, voltage, temp,
            read_num("/sys/class/backlight/panel0-backlight/brightness"), display);
    fclose(f);
}

// ---- tablet mode ----
static int keyboards, tablet_pending = 1;

static void apply_tablet_mode(void) {
    char bus[512];
    if (!tablet_pending || find_session_bus(bus, sizeof bus) < 0)
        return;                                     // the desktop is not up yet: try again later
    tablet_pending = 0;
    const char *cmd[] = { "kwriteconfig6", "--notify", "--file", "kwinrc", "--group", "Input", "--key", "TabletMode",
                          keyboards ? "off" : "on", NULL };
    say("tablet mode %s", keyboards ? "off" : "on");
    run_as_user(cmd);
}

static void keyboard_changed(int delta) {
    keyboards += delta;
    if (keyboards < 0)
        keyboards = 0;
    tablet_pending = 1;
}

static int is_cover_keyboard(const char *name) {
    return strstr(name, "Keyboard Pack") && !strstr(name, "Touchpad");
}

// ---- kernel events ----
static struct {
    unsigned maj, min;
    struct props p;
    int used, keyboard;
} known[MAX_EVENTS];

static int event_number(const char *ev) {
    int n = atoi(ev + 5);
    return n >= 0 && n < MAX_EVENTS ? n : -1;
}

static void announce_input(const char *action, const char *devpath, const char *ev, unsigned maj, unsigned min,
                           const struct props *p) {
    struct pairs out = { .n = 0 };
    put(&out, "ACTION=%s", action);
    put(&out, "DEVPATH=%s", devpath);
    put(&out, "SUBSYSTEM=input");
    put(&out, "DEVNAME=/dev/input/%s", ev);
    put(&out, "MAJOR=%u", maj);
    put(&out, "MINOR=%u", min);
    put(&out, "ID_INPUT=1");
    put(&out, "ID_SEAT=seat0");
    put(&out, "TAGS=:seat:");
    put(&out, "CURRENT_TAGS=:seat:");
    for (int i = 0; i < p->n; i++)
        put(&out, "%s=%s", p->key[i], p->value[i]);
    send_event(&out, "input", NULL);
    clear_pairs(&out);
}

static void input_add(const char *ev, const char *devpath, unsigned maj, unsigned min) {
    char name[128], node[64];
    struct props p;
    if (classify(ev, name, sizeof name, &p) < 0) {
        say("skip %s %s", ev, name);
        return;
    }
    snprintf(node, sizeof node, "/dev/input/%s", ev);
    make_dirs("/dev/input");
    drop(node);
    if (mknod(node, S_IFCHR | 0660, makedev(maj, min)) < 0)
        say("mknod %s: %s", node, strerror(errno));
    if (chown(node, DESKTOP_UID, DESKTOP_GID) < 0)
        say("chown %s: %s", node, strerror(errno));
    int gestures = is_pen_gestures(name);
    if (gestures)
        apply_pen_keys(node);
    write_input_db(maj, min, &p);
    int i = event_number(ev);
    if (i >= 0) {
        known[i].maj = maj;
        known[i].min = min;
        known[i].p = p;
        known[i].used = 1;
        known[i].keyboard = is_cover_keyboard(name);
        if (known[i].keyboard)
            keyboard_changed(1);
    }
    announce_input("add", devpath, ev, maj, min, &p);
    say("add %s %s", ev, name);
    if (gestures)
        take_gestures(node);
}

static void input_remove(const char *ev, const char *devpath) {
    int i = event_number(ev);
    if (i < 0 || !known[i].used)
        return;
    char node[64], db[64];
    announce_input("remove", devpath, ev, known[i].maj, known[i].min, &known[i].p);
    snprintf(node, sizeof node, "/dev/input/%s", ev);
    snprintf(db, sizeof db, "/run/udev/data/c%u:%u", known[i].maj, known[i].min);
    drop(node);
    drop(db);
    if (known[i].keyboard)
        keyboard_changed(-1);
    known[i].used = 0;
    say("remove %s", ev);
}

struct uevent {
    char *pair[MAX_PAIRS];
    int n;
};

static const char *uget(const struct uevent *u, const char *key) {
    size_t l = strlen(key);
    for (int i = 0; i < u->n; i++)
        if (!strncmp(u->pair[i], key, l) && u->pair[i][l] == '=')
            return u->pair[i] + l + 1;
    return NULL;
}

// Copy the kernel's pairs, leaving out the ones the caller replaces.
static void copy_except(const struct uevent *u, struct pairs *out, const char *skip1, const char *skip2) {
    for (int i = 0; i < u->n; i++) {
        size_t l1 = strlen(skip1), l2 = strlen(skip2);
        if ((!strncmp(u->pair[i], skip1, l1) && u->pair[i][l1] == '=') ||
            (!strncmp(u->pair[i], skip2, l2) && u->pair[i][l2] == '='))
            continue;
        put(out, "%s", u->pair[i]);
    }
}

static void handle_drm(const struct uevent *u) {
    const char *devname = uget(u, "DEVNAME"), *action = uget(u, "ACTION");
    if (!devname || !action || strcmp(action, "change") || strncmp(devname, "dri/card", 8))
        return;                                     // Qualcomm's DP driver sends no HOTPLUG=1
    struct pairs out = { .n = 0 };
    copy_except(u, &out, "DEVNAME", "SEQNUM");
    put(&out, "DEVNAME=/dev/%s", devname);
    put(&out, "HOTPLUG=1");
    put(&out, "TAGS=:seat:");
    put(&out, "CURRENT_TAGS=:seat:");
    send_event(&out, "drm", uget(u, "DEVTYPE"));
    clear_pairs(&out);
    say("display event %s %s %s", devname, uget(u, "name") ? uget(u, "name") : "-",
        uget(u, "status") ? uget(u, "status") : "-");
}

static void handle_block(const struct uevent *u) {
    const char *devname = uget(u, "DEVNAME"), *action = uget(u, "ACTION"), *devpath = uget(u, "DEVPATH");
    const char *maj_s = uget(u, "MAJOR"), *min_s = uget(u, "MINOR");
    if (!devname || !action || !devpath || !maj_s || !min_s)
        return;
    if (strncmp(devname, "sd", 2) || !strstr(devpath, "/usb"))
        return;                                     // never the tablet's own flash
    unsigned maj = (unsigned)atoi(maj_s), min = (unsigned)atoi(min_s);
    char node[64], db[64];
    snprintf(node, sizeof node, "/dev/%s", devname);
    snprintf(db, sizeof db, "/run/udev/data/b%u:%u", maj, min);
    struct pairs out = { .n = 0 };
    copy_except(u, &out, "DEVNAME", "SEQNUM");
    put(&out, "DEVNAME=%s", node);
    put(&out, "ID_BUS=usb");
    put(&out, "ID_USB_DRIVER=usb-storage");
    put(&out, "TAGS=:systemd:");
    put(&out, "CURRENT_TAGS=:systemd:");
    if (!strcmp(action, "add")) {
        drop(node);
        if (mknod(node, S_IFBLK | 0660, makedev(maj, min)) < 0)
            say("mknod %s: %s", node, strerror(errno));
        if (chown(node, 0, DISK_GID) < 0)
            say("chown %s: %s", node, strerror(errno));
        usleep(300000);
        make_dirs("/run/udev/data");
        FILE *f = fopen(db, "w");
        if (f)
            fputs("I:1\nE:ID_BUS=usb\nE:ID_USB_DRIVER=usb-storage\n", f);
        char cmd[128], line[256], fstype[32] = "-";
        snprintf(cmd, sizeof cmd, "blkid -p -o udev %s 2>/dev/null", node);
        FILE *b = popen(cmd, "r");
        while (b && fgets(line, sizeof line, b)) {
            line[strcspn(line, "\n")] = 0;
            if (!strchr(line, '='))
                continue;
            put(&out, "%s", line);
            if (f && (!strncmp(line, "ID_FS_", 6) || !strncmp(line, "ID_PART_", 8)))
                fprintf(f, "E:%s\n", line);
            if (!strncmp(line, "ID_FS_TYPE=", 11))
                snprintf(fstype, sizeof fstype, "%s", line + 11);
        }
        if (b)
            pclose(b);
        if (f)
            fclose(f);
        send_event(&out, "block", uget(u, "DEVTYPE"));
        say("block add %s %s", devname, fstype);
    } else if (!strcmp(action, "remove")) {
        send_event(&out, "block", uget(u, "DEVTYPE"));
        drop(node);
        drop(db);
        say("block remove %s", devname);
    }
    clear_pairs(&out);
}

static void handle_sound(const struct uevent *u) {
    const char *devname = uget(u, "DEVNAME"), *action = uget(u, "ACTION"), *devpath = uget(u, "DEVPATH");
    const char *maj_s = uget(u, "MAJOR"), *min_s = uget(u, "MINOR");
    if (!devname || !action || !devpath || !maj_s || !min_s || !strstr(devpath, "/usb"))
        return;
    static const char *const kinds[] = { "snd/controlC", "snd/pcmC", "snd/hwC", "snd/midiC" };
    int card = -1;
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (!strncmp(devname, kinds[i], strlen(kinds[i])))
            card = atoi(devname + strlen(kinds[i]));
    if (card <= 0)
        return;                                     // card0 is the tablet's own sound card
    unsigned maj = (unsigned)atoi(maj_s), min = (unsigned)atoi(min_s);
    char node[64], db[64];
    snprintf(node, sizeof node, "/dev/%s", devname);
    snprintf(db, sizeof db, "/run/udev/data/c%u:%u", maj, min);
    struct pairs out = { .n = 0 };
    copy_except(u, &out, "DEVNAME", "SEQNUM");
    put(&out, "DEVNAME=%s", node);
    put(&out, "TAGS=:seat:");
    put(&out, "CURRENT_TAGS=:seat:");
    if (!strcmp(action, "add")) {
        make_dirs("/dev/snd");
        make_dirs("/run/udev/data");
        drop(node);
        if (mknod(node, S_IFCHR | 0660, makedev(maj, min)) < 0)
            say("mknod %s: %s", node, strerror(errno));
        if (chown(node, DESKTOP_UID, DESKTOP_GID) < 0)
            say("chown %s: %s", node, strerror(errno));
        FILE *f = fopen(db, "w");
        if (f) {
            fputs("I:1\n", f);
            fclose(f);
        }
        send_event(&out, "sound", NULL);
        say("sound add %s", devname);
        if (strstr(devname, "controlC")) {          // PipeWire waits for this flag on the card itself
            char card_path[256], cardname[16], carddb[64];
            snprintf(card_path, sizeof card_path, "%s", devpath);
            char *slash = strrchr(card_path, '/');
            if (slash) {
                *slash = 0;
                snprintf(cardname, sizeof cardname, "card%d", card);
                snprintf(carddb, sizeof carddb, "/run/udev/data/+sound:%s", cardname);
                f = fopen(carddb, "w");
                if (f) {
                    fputs("I:1\nE:SOUND_INITIALIZED=1\n", f);
                    fclose(f);
                }
                usleep(500000);
                struct pairs c = { .n = 0 };
                put(&c, "ACTION=change");
                put(&c, "DEVPATH=%s", card_path);
                put(&c, "SUBSYSTEM=sound");
                put(&c, "SOUND_INITIALIZED=1");
                put(&c, "TAGS=:seat:");
                put(&c, "CURRENT_TAGS=:seat:");
                send_event(&c, "sound", NULL);
                clear_pairs(&c);
            }
        }
    } else if (!strcmp(action, "remove")) {
        send_event(&out, "sound", NULL);
        drop(node);
        drop(db);
        say("sound remove %s", devname);
    }
    clear_pairs(&out);
}

static void handle_uevent(char *buf, ssize_t len) {
    if (strncmp(buf, "add@", 4) && strncmp(buf, "remove@", 7) && strncmp(buf, "change@", 7))
        return;
    struct uevent u = { .n = 0 };
    for (ssize_t i = (ssize_t)strlen(buf) + 1; i < len && u.n < MAX_PAIRS;) {
        size_t l = strnlen(buf + i, (size_t)(len - i));
        if (l && memchr(buf + i, '=', l))
            u.pair[u.n++] = buf + i;
        i += (ssize_t)l + 1;
    }
    const char *sub = uget(&u, "SUBSYSTEM"), *action = uget(&u, "ACTION");
    if (!sub || !action)
        return;
    if (!strcmp(sub, "power_supply")) {
        struct pairs out = { .n = 0 };              // UPower refreshes when it hears about a change
        copy_except(&u, &out, "SEQNUM", "SEQNUM");
        send_event(&out, "power_supply", NULL);
        clear_pairs(&out);
    } else if (!strcmp(sub, "drm"))
        handle_drm(&u);
    else if (!strcmp(sub, "block"))
        handle_block(&u);
    else if (!strcmp(sub, "sound"))
        handle_sound(&u);
    else if (!strcmp(sub, "input")) {
        const char *devname = uget(&u, "DEVNAME"), *devpath = uget(&u, "DEVPATH");
        const char *maj = uget(&u, "MAJOR"), *min = uget(&u, "MINOR");
        if (!devname || !devpath || strncmp(devname, "input/event", 11))
            return;
        const char *ev = devname + 6;
        if (!strcmp(action, "add") && maj && min)
            input_add(ev, devpath, (unsigned)atoi(maj), (unsigned)atoi(min));
        else if (!strcmp(action, "remove"))
            input_remove(ev, devpath);
    }
}

static int open_udev_tx(void) {
    udev_tx = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (udev_tx < 0)
        perror("netlink");
    return udev_tx;
}

static void input_devpath(const char *ev, char *out, size_t size) {
    char link[192], real[4096];
    snprintf(link, sizeof link, "/sys/class/input/%s", ev);
    if (!realpath(link, real))
        real[0] = 0;
    snprintf(out, size, "%s", strncmp(real, "/sys", 4) ? real : real + 4);
}

static int cmd_classify(void) {
    struct dirent **list;
    int n = list_events(&list);
    for (int i = 0; i < n; i++) {
        char name[128] = "";
        struct props p;
        if (classify(list[i]->d_name, name, sizeof name, &p) < 0)
            printf("%s '%s' None\n", list[i]->d_name, name);
        else {
            printf("%s '%s' ", list[i]->d_name, name);
            print_props(&p);
            putchar('\n');
        }
    }
    return 0;
}

static int cmd_init(void) {
    struct dirent **list;
    int n = list_events(&list);
    make_dirs("/run/udev/data");
    for (int i = 0; i < n; i++) {
        char name[128];
        struct props p;
        unsigned maj, min;
        if (classify(list[i]->d_name, name, sizeof name, &p) == 0 && major_minor(list[i]->d_name, &maj, &min) == 0)
            write_input_db(maj, min, &p);
    }
    return 0;
}

static int cmd_replay(int argc, char **argv) {
    if (open_udev_tx() < 0)
        return 1;
    for (int i = 2; i < argc; i++) {
        unsigned maj, min;
        char path[256];
        if (major_minor(argv[i], &maj, &min) < 0)
            continue;
        input_devpath(argv[i], path, sizeof path);
        input_add(argv[i], path, maj, min);
    }
    return 0;
}

static void remember_existing(void) {
    struct dirent **list;
    int n = list_events(&list);
    for (int i = 0; i < n; i++) {
        const char *ev = list[i]->d_name;
        char name[128], node[64];
        struct props p;
        unsigned maj, min;
        int slot = event_number(ev);
        if (slot < 0 || classify(ev, name, sizeof name, &p) < 0 || major_minor(ev, &maj, &min) < 0) {
            if (slot >= 0 && !strcmp(name, "hall_irq"))
                continue;
            continue;
        }
        known[slot].maj = maj;
        known[slot].min = min;
        known[slot].p = p;
        known[slot].used = 1;
        known[slot].keyboard = is_cover_keyboard(name);
        if (known[slot].keyboard)
            keyboard_changed(1);
        snprintf(node, sizeof node, "/dev/input/%s", ev);
        if (is_pen_gestures(name)) {
            apply_pen_keys(node);
            take_gestures(node);
        }
        if (!strcmp(name, "hall_irq")) {
            hall_fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (hall_fd < 0)
                say("cannot open the case sensor: %s", strerror(errno));
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--hash")) {
        for (int i = 2; i < argc; i++)
            printf("%s %#x\n", argv[i], murmur2(argv[i]));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--classify"))
        return cmd_classify();
    if (argc > 1 && !strcmp(argv[1], "--init"))
        return cmd_init();
    if (argc > 1 && !strcmp(argv[1], "--replay"))
        return cmd_replay(argc, argv);
    if (argc > 1) {
        fprintf(stderr, "usage: lsw-inputd [--init | --classify | --replay eventN ... | --hash text ...]\n");
        return 2;
    }

    signal(SIGCHLD, SIG_IGN);                       // desktop helpers are never waited for
    if (open_udev_tx() < 0)
        return 1;
    int rx = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK, .nl_groups = GROUP_KERNEL };
    if (rx < 0 || bind(rx, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("netlink bind");
        return 1;
    }
    open_uinput();
    open_kmsg();
    remember_existing();
    say("listening");

    int64_t kmsg_next = 0;
    for (;;) {
        struct pollfd fds[4];
        int n = 0, i_rx, i_kmsg = -1, i_hall = -1, i_gest = -1;
        int64_t now = now_ms();
        i_rx = n;
        fds[n++] = (struct pollfd){ rx, POLLIN, 0 };
        if (kmsg_fd >= 0 && now >= kmsg_next) {     // the kernel log is noisy: look at it 5 times a second
            i_kmsg = n;
            fds[n++] = (struct pollfd){ kmsg_fd, POLLIN, 0 };
        }
        if (hall_fd >= 0) {
            i_hall = n;
            fds[n++] = (struct pollfd){ hall_fd, POLLIN, 0 };
        }
        if (gesture_fd >= 0) {
            i_gest = n;
            fds[n++] = (struct pollfd){ gesture_fd, POLLIN, 0 };
        }
        int timeout = 1000;
        if (kmsg_fd >= 0 && now < kmsg_next && kmsg_next - now < timeout)
            timeout = (int)(kmsg_next - now);
        if (poll(fds, (nfds_t)n, timeout) < 0 && errno != EINTR) {
            perror("poll");
            return 1;
        }
        if (fds[i_rx].revents & POLLIN) {
            static char buf[16384];
            ssize_t len = recv(rx, buf, sizeof buf - 1, MSG_DONTWAIT);
            if (len > 0) {
                buf[len] = 0;
                handle_uevent(buf, len);
            }
        }
        if (i_kmsg >= 0 && fds[i_kmsg].revents & POLLIN) {
            drain_kmsg();
            kmsg_next = now_ms() + KMSG_INTERVAL_MS;
        }
        if (i_hall >= 0 && fds[i_hall].revents & POLLIN)
            handle_hall();
        if (i_gest >= 0 && gesture_fd >= 0 && fds[i_gest].revents & (POLLIN | POLLERR | POLLHUP))
            handle_gestures();
        check_pen_silence();
        apply_tablet_mode();
        battery_tick();
        battery_log();
    }
}
