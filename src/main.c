// yeah this is basically a ripoff htop, see if i care
// also its all in one monolithic file
// hopefully I never have to touch this again

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <syscalls.h>
#include <scheduler.h>
#include <sysinfo.h>
#include <keyboard.h>
#include <console.h>
#include <hpet.h>
#include <ansii.h>

#define MAX_ROWS        64      // the kernel wont run more tasks than this
#define PID_SCAN        512
#define REFRESH_MS      1000
#define FIRST_SAMPLE_MS 250
#define PAGE_BYTES      4096ULL

#define MAX_LINES       128
#define LINE_BYTES      1024

#define METER_MIN       16
#define METER_MAX       56
#define REGION_LINES    7

#define ON_BLACK    RGB_BG(0, 0, 0)
#define C_TEXT      ON_BLACK RGB_FG(210, 210, 210)
#define C_BOLD      ON_BLACK RGB_FG(255, 255, 255)
#define C_DIM       ON_BLACK RGB_FG(110, 110, 110)

#define C_LABEL     ON_BLACK RGB_FG(225, 45, 60)
#define C_HOT       ON_BLACK RGB_FG(255, 75, 85)
#define C_DEEP      ON_BLACK RGB_FG(150, 25, 38)
#define C_WARN      ON_BLACK RGB_FG(255, 180, 0)
#define C_HEAD      RGB_BG(110, 10, 20) RGB_FG(255, 255, 255)
#define C_HEAD_SORT RGB_BG(210, 35, 50) RGB_FG(255, 255, 255)
#define C_KEY_NAME  RGB_BG(110, 10, 20) RGB_FG(255, 255, 255)

#define SYS_POLL    7
#define POLLIN      0x0001

struct pollfd {
    int   fd;
    short events;
    short revents;
};

enum {
    SORT_CPU,
    SORT_MEM,
    SORT_PID,
    SORT_TIME,
};

typedef struct {
    user_task_info_t  info;
    user_task_usage_t usage;
    uint32_t          cpu_permille;   // share of the cpu since the sample before, in tenths of a percent
} row_t;

typedef struct {
    uint64_t pid;
    uint64_t cpu_time;
} cpu_mark_t;

// one stretch of a meter
typedef struct {
    uint32_t    permille;
    const char *color;
} meter_part_t;

static row_t      rows[MAX_ROWS];
static size_t     row_count;
static cpu_mark_t marks[MAX_ROWS];
static size_t     mark_count;

static system_memory_info_t mem;
static uint64_t   task_bytes;           // what the tasks have mapped between them
static uint32_t   cpu_used_permille;

// the kernel counts cpu time in tsc cycles
static uint64_t   cycles_seen;
static uint64_t   ms_seen;
static uint64_t   last_sample_ms;

static int        sort_by = SORT_CPU;
static size_t     scroll_top;
static uint64_t   only_pid;
static int        has_only_pid;
static volatile int quit;

static uint32_t   term_cols = 80;
static uint32_t   term_rows = 25;

static uint32_t   old_kbd_flags;
static uint32_t   old_tty_flags;
static int        have_old_kbd_flags;
static int        have_old_tty_flags;

static int        hpet_fd = -2;

// a line is built here and only written if its not what the screen already shows,
// most of them are the same from one second to the next
static char       line_buf[LINE_BYTES];
static size_t     line_len;
static uint32_t   line_col;
static uint32_t   line_row;
static char       shown[MAX_LINES][LINE_BYTES];

// TODO add this shit to blibc
static long sys_poll(struct pollfd *fds, unsigned long nfds, int timeout_ms) {
    long ret;
    asm volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)SYS_POLL), "D"(fds), "S"(nfds), "d"((long)timeout_ms)
        : "rcx", "r11", "memory"
    );
    return ret;
}

static uint64_t now_ms(void) {
    uint64_t now = 0;

    if (hpet_fd == -2)
        hpet_fd = _open("/dev/hpet", O_RDONLY);
    if (hpet_fd < 0)
        return 0;

    if (_ioctl(hpet_fd, HPET_IOCTL_GET_FEMTOSECONDS, &now) != 0)
        return 0;
    return now / femtosecondsPerMillisecond;
}

static void frame_raw(const char *s) {
    size_t n = strlen(s);
    if (line_len + n >= sizeof(line_buf))
        return;
    memcpy(line_buf + line_len, s, n);
    line_len += n;
}

// text that takes up room on the line, anything past the right edge is dropped
static void frame_text(const char *color, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    size_t n = strlen(tmp);
    if (line_col + 1 >= term_cols)
        return;
    if (line_col + n > term_cols - 1)
        n = term_cols - 1 - line_col;
    tmp[n] = '\0';

    frame_raw(color);
    frame_raw(tmp);
    line_col += (uint32_t)n;
}

// spaces up to a column, in whatever colour is given
static void frame_pad_to(const char *color, uint32_t col) {
    if (col + 1 > term_cols) col = term_cols - 1;
    if (line_col >= col)
        return;

    frame_raw(color);
    while (line_col < col) {
        frame_raw(" ");
        line_col++;
    }
}

// fill to the edge so whatever was on this line before is gone. the last column is left
// alone, writing there would wrap
static void frame_endline(const char *fill) {
    frame_pad_to(fill, term_cols - 1);
    line_buf[line_len] = '\0';

    if (line_row < MAX_LINES && strcmp(line_buf, shown[line_row]) != 0) {
        tty_cursor_t at = { .x = 0, .y = line_row };
        _ioctl(1, TTY_IOCTL_SET_CURSOR, &at);
        _write(1, line_buf, (long)line_len);
        memcpy(shown[line_row], line_buf, line_len + 1);
    }

    line_len = 0;
    line_col = 0;
    line_row++;
}

// Lbl[|||||||      text]
static void frame_meter(const char *label, const meter_part_t *parts, size_t count,
                        const char *text, uint32_t width) {
    size_t text_len = strlen(text);
    if (text_len > width) text_len = width;

    frame_text(C_LABEL, "  %s", label);
    frame_text(C_BOLD, "[");
    if (line_col + width + 2 >= term_cols)
        return;

    const char *color = NULL;
    for (uint32_t i = 0; i < width; i++) {
        const char *want = C_DIM;
        char cell[2] = { ' ', '\0' };

        if (i >= width - text_len) {
            cell[0] = text[i - (width - text_len)];
        } else {
            uint32_t reach = 0;
            for (size_t p = 0; p < count; p++) {
                reach += parts[p].permille;
                if (i < (reach * width + 500) / 1000) {
                    cell[0] = '|';
                    want = parts[p].color;
                    break;
                }
            }
        }

        if (want != color) {
            frame_raw(want);
            color = want;
        }
        frame_raw(cell);
    }
    line_col += width;

    frame_text(C_BOLD, "]");
}

static char state_letter(task_state_t state) {
    switch (state) {
        case TASK_RUNNING: return 'R';
        case TASK_READY:   return 'X';
        case TASK_BLOCKED: return 'S';
        case TASK_STOPPED: return 'T';
        case TASK_ZOMBIE:  return 'Z';
        case TASK_DEAD:    return 'X';
        default:           return '?';
    }
}

static void format_bytes(char *out, size_t size, uint64_t bytes) {
    uint64_t kib = bytes / 1024;

    if (kib < 1024)
        snprintf(out, size, "%lluK", kib);
    else if (kib < 1024 * 1024)
        snprintf(out, size, "%llu.%lluM", kib / 1024, (kib % 1024) * 10 / 1024);
    else
        snprintf(out, size, "%llu.%lluG", kib / (1024 * 1024), (kib % (1024 * 1024)) * 10 / (1024 * 1024));
}

static void format_cpu_time(char *out, size_t size, uint64_t cycles) {
    if (!cycles_seen || !ms_seen) {
        snprintf(out, size, "-");
        return;
    }

    uint64_t cs = cycles / (cycles_seen / ms_seen ? cycles_seen / ms_seen : 1) / 10;
    snprintf(out, size, "%llu:%02llu.%02llu", cs / 6000, (cs / 100) % 60, cs % 100);
}

static uint64_t mark_find(uint64_t pid, uint64_t fallback) {
    for (size_t i = 0; i < mark_count; i++) {
        if (marks[i].pid == pid)
            return marks[i].cpu_time;
    }
    return fallback;
}

static int row_before(const row_t *a, const row_t *b) {
    if (sort_by == SORT_CPU && a->cpu_permille != b->cpu_permille)
        return a->cpu_permille > b->cpu_permille;
    if (sort_by == SORT_MEM && a->usage.mem_pages != b->usage.mem_pages)
        return a->usage.mem_pages > b->usage.mem_pages;
    if (sort_by == SORT_TIME && a->usage.cpu_time != b->usage.cpu_time)
        return a->usage.cpu_time > b->usage.cpu_time;
    return a->info.id < b->info.id;
}

static void sort_rows(void) {
    for (size_t i = 1; i < row_count; i++) {
        row_t row = rows[i];
        size_t j = i;
        while (j > 0 && row_before(&row, &rows[j - 1])) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = row;
    }
}

// read every task and work out what each one used since the last time we looked
static void sample(void) {
    uint64_t deltas[MAX_ROWS];
    uint64_t total = 0;
    uint64_t want = _taskcount();

    row_count = 0;
    task_bytes = 0;
    for (uint64_t pid = 0; pid < PID_SCAN && row_count < want && row_count < MAX_ROWS; pid++) {
        row_t *row = &rows[row_count];
        if (_taskinfo(pid, &row->info) != 0)
            continue;
        if (_taskusage(pid, &row->usage) != 0)
            memset(&row->usage, 0, sizeof(row->usage));

        // a pid we havent seen, or one thats been handed to a new task, we will assume its new
        uint64_t before = mark_find(pid, row->usage.cpu_time);
        deltas[row_count] = row->usage.cpu_time >= before ? row->usage.cpu_time - before : 0;
        total += deltas[row_count];
        task_bytes += row->usage.mem_pages * PAGE_BYTES;
        row_count++;
    }

    // the kernels own tasks spin when theres nothing to run, their share is the idle time
    uint64_t idle = 0;
    for (size_t i = 0; i < row_count; i++) {
        rows[i].cpu_permille = total ? (uint32_t)(deltas[i] * 1000 / total) : 0;
        if (rows[i].info.privilege_level == PRIVILEGE_KERNEL)
            idle += deltas[i];

        marks[i].pid = rows[i].info.id;
        marks[i].cpu_time = rows[i].usage.cpu_time;
    }
    mark_count = row_count;
    cpu_used_permille = total ? (uint32_t)((total - idle) * 1000 / total) : 0;

    uint64_t now = now_ms();
    if (last_sample_ms && now > last_sample_ms) {
        cycles_seen += total;
        ms_seen += now - last_sample_ms;
    }
    last_sample_ms = now;

    if (_meminfo(&mem) != 0)
        memset(&mem, 0, sizeof(mem));

    sort_rows();
}

static void fetch_size(void) {
    tty_winsize_t ws = {0};
    if (_ioctl(1, TTY_IOCTL_GET_WINSIZE, &ws) == 0 && ws.cols > 0 && ws.rows > 0) {
        term_cols = ws.cols;
        term_rows = ws.rows;
    }
}

static uint32_t meter_width(void) {
    uint32_t width = term_cols / 2 > 8 ? term_cols / 2 - 8 : METER_MIN;
    if (width < METER_MIN) width = METER_MIN;
    if (width > METER_MAX) width = METER_MAX;
    return width;
}

static void draw_meters(uint64_t in_use, uint64_t in_tasks) {
    char text[48], used[16], total[16];
    uint32_t width = meter_width();
    uint64_t secs = now_ms() / 1000;
    size_t running = 0;

    for (size_t i = 0; i < row_count; i++) {
        if (rows[i].info.state == TASK_RUNNING || rows[i].info.state == TASK_READY)
            running++;
    }

    meter_part_t cpu[] = { { cpu_used_permille, C_HOT } };
    snprintf(text, sizeof(text), "%u.%u%%", cpu_used_permille / 10, cpu_used_permille % 10);
    frame_meter("CPU", cpu, 1, text, width);
    frame_text(C_LABEL, "   Tasks: ");
    frame_text(C_BOLD, "%llu", (uint64_t)row_count);
    frame_text(C_TEXT, ", ");
    frame_text(C_HOT, "%llu", (uint64_t)running);
    frame_text(C_TEXT, " running");
    frame_endline(ON_BLACK);

    // the bright part is what the tasks hold, the dark part is the kernels share of whats left
    uint64_t usable = mem.MEMORY_USABLE ? mem.MEMORY_USABLE : 1;
    meter_part_t ram[] = {
        { (uint32_t)(in_tasks * 1000 / usable), C_HOT },
        { (uint32_t)((in_use - in_tasks) * 1000 / usable), C_DEEP },
    };
    format_bytes(used, sizeof(used), in_use);
    format_bytes(total, sizeof(total), mem.MEMORY_USABLE);
    snprintf(text, sizeof(text), "%s/%s", used, total);
    frame_meter("Mem", ram, 2, text, width);
    frame_text(C_LABEL, "   Uptime: ");
    frame_text(C_BOLD, "%02llu:%02llu:%02llu", secs / 3600, (secs / 60) % 60, secs % 60);
    frame_endline(ON_BLACK);
}

// the left column is what the total is made of, the right is everything the firmware
// kept back from it
static void draw_regions(uint64_t in_use, uint64_t in_tasks) {
    struct {
        const char *name;
        uint64_t    bytes;
        const char *color;
        int         unavailable;
    } left[] = {
        { "Usable",   mem.MEMORY_USABLE, C_BOLD, 0 },
        { "  tasks",  in_tasks, C_HOT, 0 },
        { "  kernel", in_use - in_tasks, C_DEEP, 0 },
        { "  free",   mem.MEMORY_FREE, C_TEXT, 0 },
    }, right[] = {
        { "kernel image", mem.MEMORY_KERNEL, C_TEXT, 0 },
        { "bootloader",   mem.MEMORY_BOOTLOADER, C_TEXT, 0 },
        { "acpi",         mem.MEMORY_ACPI, C_TEXT, 0 },
        { "framebuffer",  mem.MEMORY_FRAMEBUFFER, C_TEXT, 0 },
        { "reserved",     mem.MEMORY_OTHER, C_TEXT, 0 },
        { "bad",          mem.MEMORY_FAULTY, C_WARN, 1 },
    };
    char size[16];

    for (size_t i = 0; i < REGION_LINES; i++) {
        if (i < sizeof(left) / sizeof(left[0])) {
            format_bytes(size, sizeof(size), left[i].bytes);
            frame_text(left[i].color, "  %-10s", left[i].name);
            frame_text(C_TEXT, "%8s", size);
        }
        frame_pad_to(ON_BLACK, 28);

        if (i == 0) {
            frame_text(C_LABEL, "Reserved Memory");
        } else {
            format_bytes(size, sizeof(size), right[i - 1].bytes);
            frame_text(right[i - 1].color, "  %-13s", right[i - 1].name);
            frame_text(C_TEXT, "%8s", size);
            if (right[i - 1].unavailable)
                frame_text(C_WARN, "  unavailable");
        }
        frame_endline(ON_BLACK);
    }
}

static void header_cell(int sort, const char *fmt, const char *name) {
    frame_text(sort == sort_by ? C_HEAD_SORT : C_HEAD, fmt, name);
}

static void draw_row(const row_t *row) {
    char cpu[16], share[16], res[16], time[24];
    int kernel = row->info.privilege_level == PRIVILEGE_KERNEL;
    const char *color = kernel ? C_DIM : C_TEXT;
    uint64_t bytes = row->usage.mem_pages * PAGE_BYTES;
    uint64_t mem_permille = mem.MEMORY_USABLE ? bytes * 1000 / mem.MEMORY_USABLE : 0;

    snprintf(cpu, sizeof(cpu), "%u.%u", row->cpu_permille / 10, row->cpu_permille % 10);
    snprintf(share, sizeof(share), "%llu.%llu", mem_permille / 10, mem_permille % 10);
    format_bytes(res, sizeof(res), bytes);
    format_cpu_time(time, sizeof(time), row->usage.cpu_time);

    frame_text(color, " %5llu %5llu %-4s ", row->info.id, row->info.ppid, kernel ? "kern" : "user");
    frame_text(state_letter(row->info.state) == 'R' ? C_HOT : color, "%c", state_letter(row->info.state));
    frame_text(color, " %5s %5s %7s %9s ", cpu, share, res, time);
    frame_text(kernel ? C_DIM : C_BOLD, "%s", row->info.name);
    frame_endline(ON_BLACK);
}

static void draw_keys(void) {
    static const char *keys[][2] = {
        { "P", "CPU%  " }, { "M", "MEM%  " }, { "N", "PID   " }, { "T", "TIME  " },
        { "Up/Dn", "Scroll" }, { "q", "Quit  " },
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        frame_text(C_BOLD, "%s%s", i ? " " : "", keys[i][0]);
        frame_text(C_KEY_NAME, "%s", keys[i][1]);
    }
    frame_endline(C_KEY_NAME);
}

static void draw(void) {
    uint64_t in_use = mem.MEMORY_USABLE > mem.MEMORY_FREE ? mem.MEMORY_USABLE - mem.MEMORY_FREE : 0;

    // shared pages are counted once for every task that maps them, so this can run over
    uint64_t in_tasks = task_bytes < in_use ? task_bytes : in_use;

    line_len = 0;
    line_col = 0;
    line_row = 0;

    draw_meters(in_use, in_tasks);
    frame_endline(ON_BLACK);
    draw_regions(in_use, in_tasks);
    frame_endline(ON_BLACK);

    header_cell(SORT_PID, " %5s", "PID");
    frame_text(C_HEAD, " %5s %-4s S ", "PPID", "MODE");
    header_cell(SORT_CPU, "%5s", "CPU%");
    frame_text(C_HEAD, " ");
    header_cell(SORT_MEM, "%5s", "MEM%");
    frame_text(C_HEAD, " %7s ", "RES");
    header_cell(SORT_TIME, "%9s", "TIME+");
    frame_text(C_HEAD, " Command");
    frame_endline(C_HEAD);

    // the last line is the keys, the tasks get whats between
    size_t listed = has_only_pid ? 1 : row_count;
    size_t room = term_rows > line_row + 1 ? term_rows - line_row - 1 : 0;
    if (listed <= room)
        scroll_top = 0;
    else if (scroll_top > listed - room)
        scroll_top = listed - room;

    size_t skipped = 0;
    for (size_t i = 0; i < row_count && line_row + 1 < term_rows; i++) {
        if (has_only_pid && rows[i].info.id != only_pid)
            continue;
        if (skipped++ < scroll_top)
            continue;
        draw_row(&rows[i]);
    }

    while (line_row + 1 < term_rows)
        frame_endline(ON_BLACK);

    draw_keys();

    // parked in the corner, out of the way of the table
    tty_cursor_t corner = { .x = term_cols - 1, .y = term_rows - 1 };
    _ioctl(1, TTY_IOCTL_SET_CURSOR, &corner);
}

// returns 1 when the screen needs a redraw
static int handle_key(const keyboard_event_t *ev) {
    if (ev->action == KEY_RELEASE)
        return 0;

    // keycodes are scancodes here, the arrows have no ascii
    switch (ev->keycode) {
    case ArrowUp:
        if (scroll_top) scroll_top--;
        return 1;
    case ArrowDown:
        scroll_top++;
        return 1;
    case PageUp:
        scroll_top = scroll_top > 10 ? scroll_top - 10 : 0;
        return 1;
    case PageDown:
        scroll_top += 10;
        return 1;
    }

    char c = tty_key_to_ascii(ev);
    if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));

    switch (c) {
    case 'q':
    case 27:
        quit = 1;
        return 0;
    case 'p':
        sort_by = SORT_CPU;
        break;
    case 'm':
        sort_by = SORT_MEM;
        break;
    case 'n':
        sort_by = SORT_PID;
        break;
    case 't':
        sort_by = SORT_TIME;
        break;
    default:
        return 0;
    }

    sort_rows();
    return 1;
}

static void sigint_handler(int sig) {
    (void)sig;
    quit = 1;
}

static void input_init(void) {
    uint32_t flags;

    // stdin is shared with the shell, it gets put back on the way out
    if (_ioctl(0, TTY_IOCTL_GET_FLAGS, &flags) == 0) {
        old_kbd_flags = flags;
        have_old_kbd_flags = 1;
    }
    flags = TTY_NONBLOCK;
    _ioctl(0, TTY_IOCTL_SET_FLAGS, &flags);

    if (_ioctl(1, TTY_IOCTL_GET_FLAGS, &flags) == 0) {
        old_tty_flags = flags;
        have_old_tty_flags = 1;
        flags &= ~(TTY_ECHO | TTY_CANNONICAL);
        _ioctl(1, TTY_IOCTL_SET_FLAGS, &flags);
    }

    struct sigaction act = {0};
    act.handler = (uintptr_t)sigint_handler;
    act.flags = SA_RESTART;
    if (sigemptyset(&act.mask) == 0)
        sigaction(SIGINT, &act, NULL);
}

static void input_restore(void) {
    if (have_old_kbd_flags)
        _ioctl(0, TTY_IOCTL_SET_FLAGS, &old_kbd_flags);
    if (have_old_tty_flags)
        _ioctl(1, TTY_IOCTL_SET_FLAGS, &old_tty_flags);
}

static void clear_screen(void) {
    static const char clear[] = "\x1b[0m\x1b[2J";
    tty_cursor_t home = {0};

    _write(1, clear, sizeof(clear) - 1);
    _ioctl(1, TTY_IOCTL_SET_CURSOR, &home);

    for (size_t i = 0; i < MAX_LINES; i++)
        shown[i][0] = '\0';
}

// asleep until a key or the timeout, whichever is first
static void wait_for_input(int timeout_ms) {
    struct pollfd fd = { .fd = 0, .events = POLLIN };
    sys_poll(&fd, 1, timeout_ms);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        char *end = NULL;
        long pid = strtol(argv[1], &end, 10);
        if (!end || *end != '\0' || pid < 0) {
            printf("taskv: '%s' isnt a pid\n", argv[1]);
            return 1;
        }
        only_pid = (uint64_t)pid;
        has_only_pid = 1;
    }

    user_task_usage_t probe;
    if (_taskusage((uint64_t)_getpid(), &probe) != 0) {
        printf("taskv: no task usage report?\n");
        return 1;
    }

    input_init();
    fetch_size();
    clear_screen();

    // cpu use is the difference between two looks, so the first frame needs a short wait
    sample();
    wait_for_input(FIRST_SAMPLE_MS);
    sample();
    draw();

    uint64_t next_ms = now_ms() + REFRESH_MS;
    while (!quit) {
        uint64_t now = now_ms();
        int left = now && now < next_ms ? (int)(next_ms - now) : 0;
        if (!now) left = REFRESH_MS;

        if (left > 0)
            wait_for_input(left);

        keyboard_event_t ev;
        int dirty = 0;
        int keys = 0;
        while (_read(0, &ev, sizeof(ev)) == (long)sizeof(ev)) {
            dirty |= handle_key(&ev);
            keys = 1;
        }
        if (quit)
            break;

        // the window its in may have been resized
        uint32_t cols = term_cols, lines = term_rows;
        fetch_size();
        if (cols != term_cols || lines != term_rows) {
            clear_screen();
            dirty = 1;
        }

        now = now_ms();
        if (now ? now >= next_ms : !keys) {
            sample();
            next_ms = now + REFRESH_MS;
            dirty = 1;
        }

        if (dirty)
            draw();
    }

    input_restore();
    clear_screen();
    return 0;
}
