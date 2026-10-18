/*
 * sim_main.c - 펌웨어(app_bmp.c, w25q32.c)를 PC 에서 그대로 실행.
 * 가상 시리얼 포트(pty)를 열고 그 경로를 출력한다. PC 스크립트는 그 경로로 접속.
 *
 *   ./mcu_sim [--rx-err 1e-5] [--tx-err 1e-5]
 */
#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <unistd.h>
#include "main.h"
#include "app_bmp.h"

extern UART_HandleTypeDef huart2;
extern double sim_rx_err_rate, sim_tx_err_rate;
extern unsigned long sim_rx_errs, sim_tx_errs, sim_prog_violations;

static void on_sig(int s)
{
    (void)s;
    fprintf(stderr, "\n[sim] injected errors: rx=%lu tx=%lu, program-without-erase=%lu\n",
            sim_rx_errs, sim_tx_errs, sim_prog_violations);
    _exit(0);
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--rx-err")) sim_rx_err_rate = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--tx-err")) sim_tx_err_rate = atof(argv[i + 1]);
    }
    srand(1234);

    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { perror("pty"); return 1; }
    const char *name = ptsname(m);

    /* slave 를 raw 로 설정하고 열어둔다 (PC 쪽이 닫았다 열어도 master 가 EIO 나지 않도록) */
    int s = open(name, O_RDWR | O_NOCTTY);
    struct termios t;
    tcgetattr(s, &t); cfmakeraw(&t); tcsetattr(s, TCSANOW, &t);
    struct termios tm;
    tcgetattr(m, &tm); cfmakeraw(&tm); tcsetattr(m, TCSANOW, &tm);

    huart2.fd = m;
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    printf("%s\n", name);
    fflush(stdout);
    fprintf(stderr, "[sim] MCU simulator ready on %s (rx_err=%g tx_err=%g)\n",
            name, sim_rx_err_rate, sim_tx_err_rate);

    App_BMP_Run();
    return 0;
}
