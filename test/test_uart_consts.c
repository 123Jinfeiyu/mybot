/* 实证 HAL.lua uart/errno 段硬编码常量与系统头文件一致（test_ioctl_nums.c 同款方法）。
 *
 * 只打印事实，断言在 test_abi_consts.lua。输出约定：每行严格 `key = value`。
 * 编译：cc -Wall -o <out> test/test_uart_consts.c
 */
#include <stdio.h>
#include <stddef.h>
#include <termios.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

int main(void) {
	/* ---- struct termios 布局（HAL 以 cj_termios 复刻，必须逐字段同构）---- */
	printf("sizeof_termios = %zu\n", sizeof(struct termios));
	printf("offsetof_c_line = %zu\n", offsetof(struct termios, c_line));
	printf("offsetof_c_cc = %zu\n", offsetof(struct termios, c_cc));
	printf("offsetof_c_ispeed = %zu\n", offsetof(struct termios, c_ispeed));
	printf("offsetof_c_ospeed = %zu\n", offsetof(struct termios, c_ospeed));
	printf("NCCS = %d\n", NCCS);

	/* ---- c_cc 下标（HAL.UART_VMIN_IDX / UART_VTIME_IDX）---- */
	printf("VMIN = %d\n", VMIN);
	printf("VTIME = %d\n", VTIME);

	/* ---- open flags（HAL.UART_O_FLAGS = O_RDWR|O_NOCTTY|O_NONBLOCK）---- */
	printf("O_RDWR = %d\n", O_RDWR);
	printf("O_NOCTTY = %d\n", O_NOCTTY);
	printf("O_NONBLOCK = %d\n", O_NONBLOCK);

	/* ---- tcsetattr/tcflush 动作码与 c_cflag 位（HAL.UART_CREAD_CLOCAL）---- */
	printf("TCSANOW = %d\n", TCSANOW);
	printf("TCIFLUSH = %d\n", TCIFLUSH);
	printf("CREAD = %d\n", CREAD);
	printf("CLOCAL = %d\n", CLOCAL);
	printf("CS8 = %d\n", CS8);

	/* ---- 波特率 speed_t：Linux UAPI 常量（非 Hz！cfsetispeed(&t, 9600) 实测返回
	 * -1/EINVAL 且不改动 c_ispeed，只有 B9600=13 这类常量才被接受）。
	 * 各波特率逐个一行打印，供 test_abi_consts.lua 与 HAL.UART_BAUD_SPT 整表比对。 ---- */
	printf("B1200 = %d\n", B1200);
	printf("B2400 = %d\n", B2400);
	printf("B4800 = %d\n", B4800);
	printf("B9600 = %d\n", B9600);
	printf("B19200 = %d\n", B19200);
	printf("B38400 = %d\n", B38400);
	printf("B57600 = %d\n", B57600);
	printf("B115200 = %d\n", B115200);
	printf("B230400 = %d\n", B230400);
	printf("B460800 = %d\n", B460800);
	printf("B921600 = %d\n", B921600);

	/* ---- errno（HAL 以常量区分可重试与硬错误）---- */
	printf("EAGAIN = %d\n", EAGAIN);
	printf("EINTR = %d\n", EINTR);
	printf("ENOENT = %d\n", ENOENT);
	printf("EACCES = %d\n", EACCES);
	printf("EINVAL = %d\n", EINVAL);
	return 0;
}
