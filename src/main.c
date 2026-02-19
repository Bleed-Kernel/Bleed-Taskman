#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <syscalls/exit.h>
#include <scheduler.h>
#include <syscalls/taskcount.h>
#include <syscalls/taskinfo.h>
#include <syscalls/mmap.h>
#include <display.h>
#include <ansii.h>

// this should be in another file but the error squiggles are annoying as owt

#define UNICODE_TABLE   "────────────────────────────────────────────"
#define UNICODE_COR_UL  "└"
#define UNICODE_COR_UR  "┘"
#define UNICODE_COR_DL  "┌"
#define UNICODE_COR_DR  "┐"
#define UNICODE_T_JUNCR "├"
#define UNICODE_T_JUNCL "┤"
#define UNICODE_SIDE    "│"

#define TABLE           "|------------------------------------------------------|"
#define SIDE            "|"

#define BORDER_FG   RGB_FG(120, 120, 120)
#define HEADER_FG   WHITE_FG
#define UI_PID      RGB_FG(0, 255, 170)
#define UI_QUANTUM  RGB_FG(180, 200, 255)

static const char* state_plain(task_state_t state) {
    switch (state) {
        case TASK_RUNNING: return "RUNNING";
        case TASK_READY:   return "READY";
        case TASK_BLOCKED: return "WAITING";
        case TASK_DEAD:    return "DEAD";
        default:           return "UNKNOWN";
    }
}

static const char* state_color(task_state_t state) {
    switch (state) {
        case TASK_RUNNING: return RGB_FG(80, 255, 120);
        case TASK_READY:   return RGB_FG(120, 200, 255);
        case TASK_BLOCKED: return RGB_FG(255, 200, 80);
        case TASK_DEAD:    return RGB_FG(255, 100, 100);
        default:           return RGB_FG(160, 160, 160);
    }
}

int main() {
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
    printf(BORDER_FG UNICODE_COR_DL UNICODE_TABLE UNICODE_COR_DR "\n" RESET);
    printf(BORDER_FG UNICODE_SIDE RESET HEADER_FG "%-4s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-16s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-10s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-4s" RESET BORDER_FG UNICODE_SIDE RESET
           HEADER_FG "%-6s" RESET BORDER_FG UNICODE_SIDE "\n" RESET,
           "PID", "PROCESS NAME", "STATUS", "PRIV", "QUANTM");
    printf(BORDER_FG UNICODE_T_JUNCR UNICODE_TABLE UNICODE_T_JUNCL "\n" RESET);

    char user_priv[32];
    char kernel_priv[32];

    snprintf(user_priv, sizeof(user_priv), "%sUSER%s", RGB_FG(180, 200, 255), RESET);
    snprintf(kernel_priv, sizeof(kernel_priv), "%sSUPV%s", RGB_FG(255, 100, 100), RESET);


    uint64_t found = 0;
    for (uint64_t pid = 0; pid < 256 && found < total; pid++) {
        user_task_info_t *slot = &list[found];
        if (_taskinfo(pid, slot) == 0) {
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(UI_PID "%-4llu" RESET, slot->id);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(WHITE_FG "%-16s" RESET, slot->name);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf("%s%-10s" RESET, state_color(slot->state), state_plain(slot->state));
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(RGB_FG(200, 180, 255) "%-12s" RESET, 
                slot->privilege_level == 0 ? kernel_priv : user_priv);
            printf(BORDER_FG UNICODE_SIDE RESET);
            printf(UI_QUANTUM "%-6u" RESET, slot->quantum_remaining);
            printf(BORDER_FG UNICODE_SIDE "\n" RESET);
            found++;
        }
    }

    printf(BORDER_FG UNICODE_COR_UL UNICODE_TABLE UNICODE_COR_UR "\n" RESET);
    printf(RGB_FG(120, 200, 120) "%llu Tasks in-queue\n" RESET, found);

    _exit(0);
}
