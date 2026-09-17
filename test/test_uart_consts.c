/* HAL.lua uart 段硬编码常量与系统头文件的一致性自检（F-24，test_ioctl_nums.c 同款方法）
 * 编译期断言：termios 布局/波特率常量/打开标志漂移在构建期即失败。
 * 期望值出处：modules/lib/HAL.lua UART 段（cj_termios 断言与 UART_B_CONST 映射）。 */
#include <stdio.h>
#include <stddef.h>
#include <termios.h>
#include <fcntl.h>

_Static_assert(sizeof(struct termios) == 60,
	"sizeof(struct termios) != 60 (HAL.lua cj_termios 断言)");
_Static_assert(offsetof(struct termios, c_line) == 16,
	"offsetof(termios, c_line) != 16 (HAL.lua)");
_Static_assert(offsetof(struct termios, c_cc) == 17,
	"offsetof(termios, c_cc) != 17 (HAL.lua)");

#if VMIN != 6
#error "VMIN != 6 (HAL.UART_VMIN_IDX)"
#endif
#if VTIME != 5
#error "VTIME != 5 (HAL.UART_VTIME_IDX)"
#endif

/* UART_B_CONST：Hz → glibc speed_t 常量（HAL.lua 映射表） */
#if B1200 != 9
#error "B1200 != 9 (HAL.lua UART_B_CONST)"
#endif
#if B2400 != 11
#error "B2400 != 11 (HAL.lua UART_B_CONST)"
#endif
#if B4800 != 12
#error "B4800 != 12 (HAL.lua UART_B_CONST)"
#endif
#if B9600 != 13
#error "B9600 != 13 (HAL.lua UART_B_CONST)"
#endif
#if B19200 != 14
#error "B19200 != 14 (HAL.lua UART_B_CONST)"
#endif
#if B38400 != 15
#error "B38400 != 15 (HAL.lua UART_B_CONST)"
#endif
#if B57600 != 0x1001
#error "B57600 != 0x1001 (HAL.lua UART_B_CONST)"
#endif
#if B115200 != 0x1002
#error "B115200 != 0x1002 (HAL.lua UART_B_CONST)"
#endif
#if B230400 != 0x1003
#error "B230400 != 0x1003 (HAL.lua UART_B_CONST)"
#endif
#if B460800 != 0x1004
#error "B460800 != 0x1004 (HAL.lua UART_B_CONST)"
#endif
#if B500000 != 0x1005
#error "B500000 != 0x1005 (HAL.lua UART_B_CONST)"
#endif
#if B921600 != 0x1007
#error "B921600 != 0x1007 (HAL.lua UART_B_CONST)"
#endif
#if B1000000 != 0x1008
#error "B1000000 != 0x1008 (HAL.lua UART_B_CONST)"
#endif

#if (O_RDWR | O_NOCTTY | O_NONBLOCK) != 2306
#error "O_RDWR|O_NOCTTY|O_NONBLOCK != 2306 (HAL.UART_O_FLAGS)"
#endif
#if TCSANOW != 0
#error "TCSANOW != 0 (HAL.UART_TCSANOW)"
#endif
#if TCIFLUSH != 0
#error "TCIFLUSH != 0 (HAL.UART_TCIFLUSH)"
#endif
#if (CREAD | CLOCAL) != 2176
#error "CREAD|CLOCAL != 2176 (HAL.UART_CREAD_CLOCAL)"
#endif

int main(void) {
	printf("uart 常量自检通过：termios 布局/波特率常量/打开标志均与系统头文件一致\n");
	return 0;
}
