# 펌웨어 소스를 PC 에서 빌드 (host_sim/main.h 가 CubeMX main.h 를 대신함)
FW      := ../firmware/Core
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -I. -I$(FW)/Inc
SRCS    := sim_main.c sim_hal.c $(FW)/Src/app_bmp.c $(FW)/Src/w25q32.c

mcu_sim: $(SRCS) $(wildcard $(FW)/Inc/*.h) main.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f mcu_sim

.PHONY: clean
