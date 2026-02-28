#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <syscalls/exit.h>
#include <scheduler.h>
#include <syscalls/taskcount.h>
#include <syscalls/taskinfo.h>
#include <syscalls/mmap.h>
#include <ansii.h>

#define UNICODE_COR_UL  "└"
#define UNICODE_COR_UR  "┘"
#define UNICODE_COR_DL  "┌"
#define UNICODE_COR_DR  "┐"
#define UNICODE_T_JUNCR "├"
#define UNICODE_T_JUNCL "┤"
#define UNICODE_SIDE    "│"

#define BORDER_FG   RGB_FG(120, 120, 120)
#define HEADER_FG   WHITE_FG
#define UI_PID      RGB_FG(0, 255, 170)
#define UI_QUANTUM  RGB_FG(180, 200, 255)

enum {
    COL_PID = 6,
    COL_NAME = 24,
    COL_STATE = 9,
    COL_PPID = 6,
    COL_PGID = 6,
    COL_SID = 6,
    COL_PRIV = 10,
    COL_QUANTUM = 8,
    COL_COUNT = 8
};

static size_t table_inner_width(void) {
    return COL_PID + COL_NAME + COL_STATE + COL_PPID +
           COL_PGID + COL_SID + COL_PRIV + COL_QUANTUM +
           (COL_COUNT - 1);
}

static void print_table_line(const char *left, const char *right) {
    size_t inner = table_inner_width();
    printf(BORDER_FG "%s", left);
    for (size_t i = 0; i < inner; ++i) {
        printf("─");
    }
    printf("%s\n" RESET, right);
}

static const char* state_plain(task_state_t state) {
    switch (state) {
        case TASK_RUNNING: return "RUNNING";
        case TASK_READY:   return "READY";
        case TASK_BLOCKED: return "BLOCKED";
        case TASK_STOPPED: return "STOPPED";
        case TASK_ZOMBIE:  return "ZOMBIE";
        case TASK_DEAD:    return "DEAD";
        case TASK_FREE:    return "FREE";
        default:           return "UNKNOWN";
    }
}

static const char* state_color(task_state_t state) {
    switch (state) {
        case TASK_RUNNING: return RGB_FG(80, 255, 120);
        case TASK_READY:   return RGB_FG(120, 200, 255);
        case TASK_BLOCKED: return RGB_FG(255, 200, 80);
        case TASK_STOPPED: return RGB_FG(255, 170, 0);
        case TASK_ZOMBIE:  return RGB_FG(255, 100, 200);
        case TASK_DEAD:    return RGB_FG(255, 100, 100);
        case TASK_FREE:    return RGB_FG(140, 140, 140);
        default:           return RGB_FG(160, 160, 160);
    }
}

int main(void) {
    uint64_t total = _taskcount();
    if (!total) {
        printf(LOG_ERROR "System idle. No tasks reported.\n");
        _exit(1);
    }

    size_t bytes = total * sizeof(user_task_info_t);
    user_task_info_t *list = (user_task_info_t*)_mmap(bytes);

    if ((uintptr_t)list < 0x1000) {
        printf(LOG_ERROR "Process list allocation failed.\n");
        _exit(1);
    }

    printf("\n");
    print_table_line(UNICODE_COR_DL, UNICODE_COR_DR);
    printf(BORDER_FG UNICODE_SIDE RESET HEADER_FG "%-6s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-24s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-9s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-6s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-6s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-6s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-10s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-8s" RESET BORDER_FG UNICODE_SIDE "\n" RESET,
           "PID", "PROCESS NAME", "STATE", "PPID", "PGID", "SID", "PRIVILEGE", "QUANTUM");
    print_table_line(UNICODE_T_JUNCR, UNICODE_T_JUNCL);

    const char *user_priv = "USER";
    const char *kernel_priv = "SUPV";

    uint64_t found = 0;
    for (uint64_t pid = 0; pid < 512 && found < total; pid++) {
        user_task_info_t *slot = &list[found];
        if (_taskinfo(pid, slot) == 0) {
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(UI_PID "%-6llu" RESET, slot->id);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(WHITE_FG "%-24s" RESET, slot->name);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf("%s%-9s" RESET, state_color(slot->state), state_plain(slot->state));
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(RGB_FG(170, 170, 170) "%-6llu" RESET, slot->ppid);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(RGB_FG(170, 170, 170) "%-6llu" RESET, slot->pgid);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(RGB_FG(170, 170, 170) "%-6llu" RESET, slot->sid);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf("%s%-10s" RESET,
                slot->privilege_level == 0 ? RGB_FG(255, 100, 100) : RGB_FG(180, 200, 255),
                slot->privilege_level == 0 ? kernel_priv : user_priv);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(UI_QUANTUM "%-8u" RESET, slot->quantum_remaining);
            printf(BORDER_FG UNICODE_SIDE "\n" RESET);
            found++;
        }
    }

    print_table_line(UNICODE_COR_UL, UNICODE_COR_UR);
    printf(RGB_FG(120, 200, 120) "%llu Tasks in-queue\n" RESET, found);

    _exit(0);
}
