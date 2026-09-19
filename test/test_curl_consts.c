/* llm_http.lua 硬编码 CURLOPT/CURLINFO 编号与 curl.h 的一致性自检（F-24 同族）
 * 全部为编译期断言：libcurl 大版本升级导致选项编号漂移时构建期即失败，
 * 而不是等到 LuaJIT 侧把错误编号传给 curl_easy_setopt 后行为诡异（如超时
 * 静默失效——见 llm_http.lua 头部「必须用定签名 cast 调用 curl_easy_setopt」）。
 * 期望值出处：modules/lib/llm_http.lua 的 OPT_URL / INFO_RESPONSE_CODE 等常量段。
 *
 * 与 test_ioctl_nums 等三个常量自检不同，本文件依赖 libcurl 开发头文件
 * （mybot 构建本身不需要它——agent 模式运行期才 dlopen libcurl.so），故
 * CMake 仅在找到 curl/curl.h 时构建本目标；run_all.sh 对缺失产物按跳过计。
 */
#include <curl/curl.h>

/* llm_http.lua：setopt_ptr(h, OPT_URL, ...) 等 —— libcurl 8.x 以
 * CURLOPT(name, type, num) 枚举宏定义选项，数值 = 类型基址 + 序号，
 * 本断言比对的是展开后的最终编号。 */
_Static_assert(CURLOPT_URL == 10002,
	"CURLOPT_URL != 10002 (llm_http.lua OPT_URL)");
_Static_assert(CURLOPT_POSTFIELDS == 10015,
	"CURLOPT_POSTFIELDS != 10015 (llm_http.lua OPT_POSTFIELDS)");
_Static_assert(CURLOPT_POSTFIELDSIZE == 60,
	"CURLOPT_POSTFIELDSIZE != 60 (llm_http.lua OPT_POSTFIELDSIZE)");
_Static_assert(CURLOPT_WRITEFUNCTION == 20011,
	"CURLOPT_WRITEFUNCTION != 20011 (llm_http.lua OPT_WRITEFUNCTION)");
_Static_assert(CURLOPT_HTTPHEADER == 10023,
	"CURLOPT_HTTPHEADER != 10023 (llm_http.lua OPT_HTTPHEADER)");
_Static_assert(CURLOPT_TIMEOUT == 13,
	"CURLOPT_TIMEOUT != 13 (llm_http.lua OPT_TIMEOUT)");
_Static_assert(CURLOPT_CONNECTTIMEOUT == 78,
	"CURLOPT_CONNECTTIMEOUT != 78 (llm_http.lua OPT_CONNECTTIMEOUT)");
_Static_assert(CURLOPT_NOSIGNAL == 99,
	"CURLOPT_NOSIGNAL != 99 (llm_http.lua OPT_NOSIGNAL)");

_Static_assert(CURLINFO_RESPONSE_CODE == 2097154,
	"CURLINFO_RESPONSE_CODE != 2097154 (llm_http.lua INFO_RESPONSE_CODE)");
_Static_assert(CURL_GLOBAL_ALL == 3,
	"CURL_GLOBAL_ALL != 3 (llm_http.lua CURL_GLOBAL_ALL)");

int main(void) {
	printf("curl 常量自检通过：CURLOPT/CURLINFO 编号与 curl.h 一致（llm_http.lua 硬编码值未漂移）\n");
	return 0;
}
