#pragma once
/* harness.hpp —— 零第三方依赖的极简单元测试骨架
 *
 * 为什么不用 Catch2/googletest：本项目 README 明示「零第三方 C++ 依赖」，
 * 且被测面只是解析器与值语义工具，几十行断言足够；引入外部框架要么下载
 * 要么 vendored，都会让「clone 完直接 cmake --build」这条路断掉。
 *
 * 输出约定与 test/run_all.sh 的守门契约一致：
 *   [PASS] <case>   单条通过
 *   [FAIL] <case>   单条失败（附表达式与行号）
 *   [DONE]          全部跑完的完成标记（缺标记 = 早退/崩溃，判失败，不假绿）
 */

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace th {

struct Case {
	std::string name;
	std::function<void()> fn;
};

inline std::vector<Case>& cases() {
	static std::vector<Case> v;
	return v;
}

inline int& failureCount() {
	static int n = 0;
	return n;
}

inline bool& currentFailed() {
	static bool b = false;
	return b;
}

struct Registrar {
	Registrar(const std::string& name, std::function<void()> fn) {
		cases().push_back(Case{name, std::move(fn)});
	}
};

inline void reportFailure(const char* expr, const char* file, int line, const std::string& extra) {
	currentFailed() = true;
	std::printf("       表达式: %s\n", expr);
	std::printf("       位置:   %s:%d\n", file, line);
	if (!extra.empty()) { std::printf("       说明:   %s\n", extra.c_str()); }
}

inline void check(bool ok, const char* expr, const char* file, int line) {
	if (!ok) { reportFailure(expr, file, line, std::string()); }
}

/* 运行全部用例，返回失败数 */
inline int runAll(const std::string& filter) {
	int passed = 0;
	failureCount() = 0;
	for (const Case& c : cases()) {
		if (!filter.empty() && c.name.find(filter) == std::string::npos) { continue; }
		currentFailed() = false;
		try {
			c.fn();
		} catch (const std::exception& e) {
			reportFailure("(未捕获异常)", __FILE__, 0,
				std::string("用例抛出异常: ") + e.what());
		} catch (...) {
			reportFailure("(未捕获异常)", __FILE__, 0, "用例抛出非 std 异常");
		}
		if (currentFailed()) {
			std::printf("[FAIL] %s\n", c.name.c_str());
			++failureCount();
		} else {
			std::printf("[PASS] %s\n", c.name.c_str());
			++passed;
		}
	}
	std::printf("C++ 单测: %d 通过, %d 失败\n", passed, failureCount());
	if (passed + failureCount() == 0) {
		std::printf("       无匹配用例（过滤器 '%s' 未命中，请检查拼写）\n", filter.c_str());
		return -1;
	}
	std::printf("[DONE]\n");
	/* C10（2026-09-20 审计）：退出码只能是 0/1，**不能**是失败用例数——进程
	 * 退出状态被 OS 截断到 8 位，256 个失败会恰好回绕成 0（实测 256 用例全红
	 * 时 `ctest -R cpp_unit` 报 "100% tests passed"）。run_all.sh 有 [FAIL]
	 * grep 兜底不受影响，但文档推荐的逐用例入口 ctest 会假绿。
	 * 约定：0 = 全通过；1 = 有失败（或过滤器无命中）。 */
	return failureCount() == 0 ? 0 : 1;
}

} // namespace th

#define TH_TEST(name) \
	static void name(); \
	static ::th::Registrar th_reg_##name(#name, &name); \
	static void name()

#define TH_CHECK(expr) ::th::check(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

/* 断言 expr，失败时附自定义说明——用于「遍历集合并点名具体违例项」的断言，
 * 否则失败信息只有表达式文本，看不出是哪一个元素越界/缺失。 */
#define TH_CHECK_MSG(expr, msg)                                            \
	do {                                                                \
		if (!static_cast<bool>(expr)) {                                 \
			::th::reportFailure(#expr, __FILE__, __LINE__, (msg));      \
		}                                                               \
	} while (false)

/* 断言 expr 抛出 Exc 类型异常（负路径契约的主力） */
#define TH_CHECK_THROWS(Exc, expr)                                        \
	do {                                                                \
		bool th_threw_ = false;                                         \
		try { (void)(expr); }                                           \
		catch (const Exc&) { th_threw_ = true; }                        \
		catch (const std::exception& th_e_) {                           \
			::th::reportFailure(#expr, __FILE__, __LINE__,              \
				std::string("抛出了错误类型，期望 ") + #Exc + "，实际 std::exception: " + th_e_.what()); \
			th_threw_ = true;                                           \
		}                                                               \
		if (!th_threw_) {                                               \
			::th::reportFailure(#expr, __FILE__, __LINE__,              \
				std::string("未抛出任何异常，期望 ") + #Exc);           \
		}                                                               \
	} while (false)

/* 断言 expr 抛 Exc 且 what() 含 sub（校验「清晰报错」而非裸崩） */
#define TH_CHECK_THROWS_MSG(Exc, sub, expr)                               \
	do {                                                                \
		bool th_threw_ = false;                                         \
		try { (void)(expr); }                                           \
		catch (const Exc& th_e_) {                                      \
			th_threw_ = true;                                           \
			if (std::string(th_e_.what()).find(sub) == std::string::npos) { \
				::th::reportFailure(#expr, __FILE__, __LINE__,          \
					std::string("报错文本不含 \"") + sub + "\"，实际: " + th_e_.what()); \
			}                                                           \
		}                                                               \
		if (!th_threw_) {                                               \
			::th::reportFailure(#expr, __FILE__, __LINE__,              \
				std::string("未抛出任何异常，期望 ") + #Exc);           \
		}                                                               \
	} while (false)
