#include "harness.hpp"

#include <string>

#include "jsonparse.hpp"
#include "util.hpp"

using namespace bot;

TH_TEST(jsonparse_scalars) {
	TH_CHECK(parseJson("null").isNull());
	TH_CHECK(parseJson(" true ").isBool());
	TH_CHECK(parseJson("true").getBool());
	TH_CHECK(!parseJson("false").getBool());
	TH_CHECK(parseJson("42").isInt());
	TH_CHECK(parseJson("42").getInt() == 42);
	TH_CHECK(parseJson("-7").getInt() == -7);
	TH_CHECK(parseJson("9223372036854775807").isInt());
	TH_CHECK(parseJson("0").isInt());
	TH_CHECK(parseJson("3.5").isFloat());
	TH_CHECK(parseJson("3.5").getFloat() == 3.5);
	TH_CHECK(parseJson("1e3").getFloat() == 1000.0);
	TH_CHECK(parseJson("1.5E-2").getFloat() == 0.015);
	TH_CHECK(parseJson("\"hi\"").isStr());
	TH_CHECK(parseJson("\"hi\"").getValue() == "hi");
}

TH_TEST(jsonparse_containers) {
	Json v = parseJson("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":2.5}}");
	TH_CHECK(v.isObj());
	TH_CHECK(v.objSize() == 3);
	TH_CHECK(v.get("a")->getInt() == 1);
	TH_CHECK(v.get("b")->isArr());
	TH_CHECK(v.get("b")->size() == 3);
	TH_CHECK(v.get("b")->at(0).getBool());
	TH_CHECK(v.get("b")->at(1).isNull());
	TH_CHECK(v.get("b")->at(2).getValue() == "x");
	TH_CHECK(v.get("c")->get("d")->getFloat() == 2.5);
	/* 字段序保持输入序（与 Json 的 put/items 语义一致） */
	TH_CHECK(v.items()[0].first == "a");
	TH_CHECK(v.items()[2].first == "c");
	/* 空容器 */
	TH_CHECK(parseJson("{}").isObj() && parseJson("{}").objSize() == 0);
	TH_CHECK(parseJson("[]").isArr() && parseJson("[]").size() == 0);
}

TH_TEST(jsonparse_string_escapes) {
	TH_CHECK(parseJson("\"a\\nb\\t\\\"c\\\\d\\/\"").getValue() == "a\nb\t\"c\\d/");
	TH_CHECK(parseJson("\"\\u0041\"").getValue() == "A");
	/* BMP 码点 → UTF-8 */
	TH_CHECK(parseJson("\"\\u4e2d\"").getValue() == "\u4e2d");
	/* 代理对 → 4 字节 UTF-8（U+1F600） */
	TH_CHECK(parseJson("\"\\ud83d\\ude00\"").getValue() == "\U0001F600");
	/* 原样 UTF-8 透传 */
	TH_CHECK(parseJson("\"\u673a\u5668\u4eba\"").getValue() == "\u673a\u5668\u4eba");
}

TH_TEST(jsonparse_rejects_malformed) {
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "offset", parseJson(""));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "trailing", parseJson("{\"a\":1}garbage"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "unterminated string", parseJson("\"abc"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "invalid escape", parseJson("\"a\\qb\""));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "lone high surrogate",
		parseJson("\"\\ud83d\""));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "lone low surrogate",
		parseJson("\"\\ude00\""));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "control character",
		parseJson("\"a\nb\""));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "trailing", parseJson("01"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "unexpected", parseJson("+1"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "unexpected", parseJson(".5"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "invalid fraction", parseJson("1."));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "unexpected", parseJson("NaN"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "expected object key", parseJson("{1:2}"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "expected ':'", parseJson("{\"a\" 1}"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "expected ']'", parseJson("[1 2]"));
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "unexpected end", parseJson("{\"a\":"));
}

TH_TEST(jsonparse_number_overflow_falls_back_to_float) {
	/* 超出 int64 的整数字面量：回落 Float 而不是让整串响应失败
	 * （调用方只读小整数，报错反而是更差的选择） */
	Json v = parseJson("12345678901234567890123");
	TH_CHECK(v.isFloat());
	TH_CHECK(v.getFloat() > 1e22);
}

TH_TEST(jsonparse_depth_limit) {
	std::string deep;
	for (int i = 0; i < 70; ++i) { deep += "["; }
	for (int i = 0; i < 70; ++i) { deep += "]"; }
	TH_CHECK_THROWS_MSG(IllegalArgumentException, "nesting too deep", parseJson(deep));

	/* 上限之下照常可解 */
	std::string ok;
	for (int i = 0; i < 40; ++i) { ok += "["; }
	ok += "1";
	for (int i = 0; i < 40; ++i) { ok += "]"; }
	TH_CHECK(parseJson(ok).isArr());
}
