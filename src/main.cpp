#include <cstdlib>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#include "actionsource.hpp"
#include "robot.hpp"
#include "util.hpp"

namespace bot {

/* ---- D4：SIGINT/SIGTERM 处理器——仅置停止标志（async-signal-safe），
 * 让主流程走正常 deactivate（驱动 destroy / 守护停机）后再退出，
 * 避免 Ctrl-C 把舵机留在最后指令位置、I2C 从设备滞留中间态。 */
extern "C" void onStopSignal(int) {
	bot::stopRequested().store(true);
}

/* ---- 命令行参数解析：--key=value 与 --key value 双形态 ---- */
static std::map<std::string, std::string> parseArgs(int argc, char** argv) {
	std::map<std::string, std::string> opts;
	for (int k = 1; k < argc; ++k) {
		std::string a = argv[k];
		if (a.rfind("--", 0) != 0) { continue; }	/* 忽略位置参数 */
		std::string key = a.substr(2);
		std::string val;
		auto eq = key.find('=');
		if (eq != std::string::npos) {
			val = key.substr(eq + 1);
			key = key.substr(0, eq);
		} else if (k + 1 < argc) {
			val = argv[++k];
		} else {
			val = "1";
		}
		opts[key] = val;
	}
	return opts;
}

void print_help() {
	std::cout << "这是一个机器人动作脚本解析器。" << std::endl;
	std::cout << "使用--robot选项指定结构脚本，然后使用管道输入动作脚本，示例：" << std::endl;
	std::cout << "cat scripts/std.mt | ./build/mybot --robot=./robots/spider1.sql" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "动作输入源（优先级从高到低）：" << std::endl;
	std::cout << "  --tcp <port>       TCP 长驻模式：监听端口等待客户端下发 .mt" << std::endl;
	std::cout << "  --tcp-host <addr>  TCP 监听地址，缺省 127.0.0.1（跨机需显式给 0.0.0.0）" << std::endl;
	std::cout << "  --tcp-token <tok>  TCP 可选口令：对端须先发一行 \"<tok>\\n\" 再发脚本" << std::endl;
	std::cout << "  --tcp-idle <sec>   TCP 空闲超时，缺省 30s（超时按输入结束处理）" << std::endl;
	std::cout << "  --input <path>     从文件读取动作脚本" << std::endl;
	std::cout << "  （缺省）           从 stdin 管道读取" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "安全约束：动作脚本只能引入新的参数键，不得覆盖结构脚本的元数据列" << std::endl;
	std::cout << "（fd / address / max / min / offset 等），且单次输入上限 8 MiB。" << std::endl;
}

} // namespace bot

int main(int argc, char** argv) {
	using namespace bot;

	auto options = parseArgs(argc, argv);

	/* ---- 拉起帮助手册 ---- */
	if (options.count("help")) {
		print_help();
		return 0;
	}

	/* ---- 结构脚本 + 动作输入源分支 ---- */
	if (options.count("robot")) {
		std::string rpath = options["robot"];

		/* 友好报错：结构脚本打不开 → 人话提示，不裸栈回溯 */
		std::unique_ptr<FileReader> fstream;
		{
			auto f = std::make_unique<FileReader>(rpath);
			if (!f->good()) {
				std::cout << "无法打开结构脚本 '" << rpath << "'" << std::endl;
				std::cout << "请确认 --robot 指向存在的结构脚本，示例：cat scripts/std.mt | ./build/mybot --robot=robots/spider1.sql" << std::endl;
				return 1;
			}
			fstream = std::move(f);
		}

		std::unique_ptr<RobotStruct> robot;
		try {
			robot = std::make_unique<RobotStruct>(*fstream);
		} catch (const std::exception& e) {
			std::cout << "结构脚本解析失败 '" << rpath << "': " << e.what() << std::endl;
			std::cout << "无硬件自检链路：cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql" << std::endl;
			return 1;
		}

		/* D4：安装 SIGINT/SIGTERM 处理器（仅置标志），Ctrl-C 走正常清理链 */
		struct sigaction sa{};
		sa.sa_handler = onStopSignal;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = 0;	/* 不用 SA_RESTART：让 accept 等阻塞点被 EINTR 打断以便检查标志 */
		::sigaction(SIGINT, &sa, nullptr);
		::sigaction(SIGTERM, &sa, nullptr);

		int rc = 0;
		try {
			robot->active();

			/* ActionSource：可替换动作输入源（stdin / TCP / 文件） */
			std::unique_ptr<Reader> input = openActionSource(options);
			if (input) {
				robot->run(*input);
			} else {
				std::cout << "动作输入源打开失败，退出" << std::endl;
				rc = 1;
			}
		} catch (const std::exception& e) {
			std::cout << "Aborted: " << e.what() << std::endl;
			rc = 1;
		}

		/* 无论有没有成功激活，都进行清理（对应 Cangjie finally） */
		robot->deactivate();
		return rc;
	}

	/* 没有输入机器人结构脚本，打印帮助文档 */
	print_help();
	return 0;
}