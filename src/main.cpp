#include <algorithm>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

/* ---- 命令行参数解析：--key=value 与 --key value 双形态 ----
 * 未知键收进 unknown（F-20：由调用方统一拒绝，退出码 2）。 */
struct ParsedArgs {
	bot::CliOptions opts;
	std::vector<std::string> unknown;
};

static ParsedArgs parseArgs(int argc, char** argv) {
	ParsedArgs out;
	for (int k = 1; k < argc; ++k) {
		std::string a = argv[k];
		if (a.rfind("--", 0) != 0) { continue; }	/* 忽略位置参数 */
		std::string key = a.substr(2);
		std::string val;
		/* 审计中-3：区分「布尔型 flag」与「带值 key」——裸写（无 '=' 且
		 * 下一参数以 -- 开头或不存在）时不再回落 val="1"。旧实现 `--tcp`
		 * 单独使用会静默得到端口 1，随后 bind 失败的报错与用户意图
		 * （漏写端口号）相去甚远。 */
		bool noValue = true;
		auto eq = key.find('=');
		if (eq != std::string::npos) {
			val = key.substr(eq + 1);
			key = key.substr(0, eq);
			noValue = false;
		} else if (k + 1 < argc && std::string(argv[k + 1]).rfind("--", 0) != 0) {
			val = argv[++k];
			noValue = false;
		}
		if (key == "robot") { out.opts.robot = noValue ? "1" : val; }
		else if (key == "help") { out.opts.help = true; }
		else if (key == "tcp") {
			out.opts.tcp = true;
			/* 裸 --tcp：tcp 置位但端口留空，由 openActionSource 报「缺端口号」 */
			if (!noValue) { out.opts.tcpPort = val; }
		}
		else if (key == "tcp-host") { out.opts.tcpHost = noValue ? "1" : val; }
		else if (key == "tcp-token") { out.opts.tcpToken = noValue ? "1" : val; }
		else if (key == "tcp-idle") { out.opts.tcpIdle = noValue ? "1" : val; }
		else if (key == "input") { out.opts.input = noValue ? "1" : val; }
		else { out.unknown.push_back(key); }
	}
	return out;
}

void print_help() {
	std::cout << "这是一个机器人动作脚本解析器。" << std::endl;
	std::cout << "使用--robot选项指定结构脚本，然后使用管道输入动作脚本，示例：" << std::endl;
	std::cout << "cat scripts/std.mt | ./build/mybot --robot=./robots/spider1.sql" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "动作输入源（优先级从高到低）：" << std::endl;
	std::cout << "  --tcp <port>       TCP 模式：监听端口接受一次连接，读取对端下发的 .mt" << std::endl;
	std::cout << "                     （EOF 或 --tcp-idle 超时按输入结束处理，非流式长驻）" << std::endl;
	std::cout << "  --tcp-host <addr>  TCP 监听地址，缺省 127.0.0.1；跨机监听（如 0.0.0.0）" << std::endl;
	std::cout << "                     必须同时提供 --tcp-token，否则拒绝启动" << std::endl;
	std::cout << "  --tcp-token <tok>  TCP 口令：跨机监听必填；对端须先发一行 \"<tok>\\n\" 再发脚本" << std::endl;
	std::cout << "  --tcp-idle <sec>   TCP 空闲超时，缺省 30s（超时按输入结束处理）" << std::endl;
	std::cout << "  --input <path>     从文件读取动作脚本" << std::endl;
	std::cout << "  （缺省）           从 stdin 管道读取" << std::endl;
	std::cout << "" << std::endl;
	std::cout << "安全约束：动作脚本只能引入新的参数键，不得覆盖结构脚本的元数据列" << std::endl;
	std::cout << "（fd / address / max / min / offset 等）；结构脚本与动作脚本单次输入上限均为 8 MiB。" << std::endl;
}

} // namespace bot

int main(int argc, char** argv) {
	using namespace bot;

	auto args = parseArgs(argc, argv);
	const CliOptions& options = args.opts;

	/* ---- 拉起帮助手册 ---- */
	if (options.help) {
		print_help();
		return 0;
	}

	/* ---- F-20：未知/拼错的 --key 一律拒绝（退出码 2）----
	 * 旧行为是静默忽略：`--robott=foo` 打印帮助后 exit 0（CI 里的假成功），
	 * `--robot` 拼错还会让程序在「没给结构脚本」的路径上看似正常退出。 */
	if (!args.unknown.empty()) {
		std::sort(args.unknown.begin(), args.unknown.end());
		for (const auto& key : args.unknown) {
			std::cerr << "未知参数：--" << key << "；用 --help 查看支持的参数" << std::endl;
		}
		return 2;
	}

	/* ---- 结构脚本 + 动作输入源分支 ---- */
	if (options.robot) {
		std::string rpath = *options.robot;

		/* 友好报错：结构脚本打不开 → 人话提示，不裸栈回溯 */
		std::unique_ptr<Reader> fstream;
		{
			auto f = std::make_unique<FileReader>(rpath);
			if (!f->good()) {
				std::cerr << "无法打开结构脚本 '" << rpath << "'" << std::endl;
				std::cerr << "请确认 --robot 指向存在的结构脚本，示例：cat scripts/std.mt | ./build/mybot --robot=robots/spider1.sql" << std::endl;
				return 1;
			}
			/* F-02：结构脚本与动作脚本共用同一字节上限——readSQL 先把全部
			 * token 驻留内存，不设限即「输入多大、进程就吃多大内存」。 */
			fstream = std::make_unique<LimitedReader>(std::move(f), kMaxScriptBytes, "structure script");
		}

		std::unique_ptr<RobotStruct> robot;
		try {
			robot = std::make_unique<RobotStruct>(*fstream);
		} catch (const std::exception& e) {
			std::cerr << "结构脚本解析失败 '" << rpath << "': " << e.what() << std::endl;
			std::cerr << "无硬件自检链路：cat scripts/relay-test.mt | ./build/mybot --robot=robots/relay-test.sql" << std::endl;
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
			/* 审计 H-2 防线：先取动作脚本、后上电。TCP 模式下 accept/口令/
			 * 读取阶段任何失败（无客户端连接、口令错、停止信号）都发生在
			 * 硬件 build 之前——不再出现「硬件已 build、带电空等客户端」；
			 * 三种输入源本就一次性读全，open 失败时 deactivate 也不会执行
			 * 任何 destroy 之外的驱动副作用。 */
			std::unique_ptr<Reader> input = openActionSource(options);
			if (!input) {
				std::cerr << "动作输入源打开失败，退出" << std::endl;
				rc = 1;
			} else {
				robot->active();

				robot->run(*input);
			}
		} catch (const std::exception& e) {
			std::cerr << "Aborted: " << e.what() << std::endl;
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