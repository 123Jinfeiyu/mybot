/* main.cpp —— C++ 单测入口
 *
 * 用法：./build/mybot_tests            跑全部
 *       ./build/mybot_tests matrix     只跑名字含 matrix 的用例
 * 退出码：失败用例数（0 = 全通过）；过滤器无命中时返回 1（不假绿）。
 */

#include <string>

#include "harness.hpp"

int main(int argc, char** argv) {
	const std::string filter = argc > 1 ? std::string(argv[1]) : std::string();
	const int rc = th::runAll(filter);
	return rc < 0 ? 1 : rc;
}
