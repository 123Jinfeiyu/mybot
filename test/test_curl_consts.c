/* llm_http.lua 硬编码 CURLOPT/CURLINFO 编号与 curl.h 的一致性自检（F-24 同族）
 * 全部为编译期断言：libcurl 大版本升级导致选项编号漂移时构建期即失败，
 * 而不是等到 LuaJIT 侧把错误编号传给 curl_easy_setopt 后行为诡异（如超时
 * 静默失效——见 llm_http.lua 头部「必须用定签名 cast 调用 curl_easy_setopt」）。
 * 期望值出处：modules/lib/llm_http.lua 的 OPT_URL / INFO_RESPONSE_CODE 等常量段。
 *
 * A2（2026-09-20 审计）修复：本文件此前把 llm_http.lua 的数字在 C 里手抄一遍，
 * 从不读 Lua 源——Lua 侧漂移完全不被发现。现在第一段断言直接钉住
 * gen_const_check.cmake 从 llm_http.lua **抽取**生成的值，第二段保留原有的
 * 「内核/库头未漂移」判据：两侧都钉死。
 *
 * 与 test_ioctl_nums 等三个常量自检不同，本文件依赖 libcurl 开发头文件
 * （mybot 构建本身不需要它——agent 模式运行期才 dlopen libcurl.so），故
 * CMake 仅在找到 curl/curl.h 时构建本目标；run_all.sh 对缺失产物按跳过计。
 */
#include <stdio.h>
#include <curl/curl.h>

#include "const_lua.h"		/* A2：配置期由 gen_const_check.cmake 从 llm_http.lua 抽取 */

/* ---- 第一段：llm_http.lua 里的实际值（此前完全未被校验）---- */
_Static_assert(LLM_CONST_OPT_URL == 10002,
	"llm_http.lua OPT_URL drifted (expected 10002)");
_Static_assert(LLM_CONST_OPT_POSTFIELDS == 10015,
	"llm_http.lua OPT_POSTFIELDS drifted (expected 10015)");
_Static_assert(LLM_CONST_OPT_POSTFIELDSIZE == 60,
	"llm_http.lua OPT_POSTFIELDSIZE drifted (expected 60)");
_Static_assert(LLM_CONST_OPT_WRITEFUNCTION == 20011,
	"llm_http.lua OPT_WRITEFUNCTION drifted (expected 20011)");
_Static_assert(LLM_CONST_OPT_HTTPHEADER == 10023,
	"llm_http.lua OPT_HTTPHEADER drifted (expected 10023)");
_Static_assert(LLM_CONST_OPT_TIMEOUT == 13,
	"llm_http.lua OPT_TIMEOUT drifted (expected 13)");
_Static_assert(LLM_CONST_OPT_CONNECTTIMEOUT == 78,
	"llm_http.lua OPT_CONNECTTIMEOUT drifted (expected 78)");
_Static_assert(LLM_CONST_OPT_NOSIGNAL == 99,
	"llm_http.lua OPT_NOSIGNAL drifted (expected 99)");
_Static_assert(LLM_CONST_INFO_RESPONSE_CODE == 2097154,
	"llm_http.lua INFO_RESPONSE_CODE drifted (expected 2097154)");
_Static_assert(LLM_CONST_CURL_GLOBAL_ALL == 3,
	"llm_http.lua CURL_GLOBAL_ALL drifted (expected 3)");

/* ---- 第二段：curl.h 未漂移（原判据，保留）---- */

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
