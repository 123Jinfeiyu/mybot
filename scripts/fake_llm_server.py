#!/usr/bin/env python3
"""离线假 OpenAI 兼容服务（仅用于 agent 模式守门，不参与构建产物）。

按「已经回传了几条 tool 消息」推进剧本，用来验证 agent 工具循环真的迭代了、
且工具结果真的回传给了模型：
    0 条 → 调 list_hardware
    1 条 → 调 run_action_script(relay-test.mt)
    2 条 → 调 run_command(echo agent-echo-ok)
    ≥3 条 → 给出最终回答

响应里带 reasoning_content：这样 DeepSeek 驱动必须把它回传到下一轮请求，
守门脚本才能在抓到的报文里断言这条有状态协议要求确实生效。

用法: python3 scripts/fake_llm_server.py <port> [capture_file] [mode] [headers_file]
      mode: ok（默认） | slow（睡 5 秒再答，用于验证 --llm-timeout 真的生效）
            | camera（先要求拍一张照片，再给最终回答——验证图片真的进了上下文）
            | manycalls[:N]（首轮返回 N 条工具调用，缺省 3；配合
              max_calls_per_round=2 触发宿主的单轮截断分支——SAN-1）
            | 429x2ok（前两次 429 + Retry-After: 1，第三次正常回答——SAN-3：
              不重试则只收到 1 个 POST 且任务失败，重试则 3 个 POST 且成功）
            | http-503（恒 503 + Retry-After: 1——SAN-3：重试用尽后如实失败）
      headers_file: 非空则把收到的请求头逐行写入（用来断言 API key 的来源）
"""
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1])
CAPTURE = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] else None
MODE = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else "ok"
HEADERS = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] else None

# 跨请求计数（429x2ok 用它决定第几次请求开始放行）。服务是单进程单线程
# （HTTPServer 非 ThreadingHTTPServer），无并发，故不需要锁。
ATTEMPTS = {"n": 0}


def tool_call(name, arguments):
    return {
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "调用工具 %s。" % name,
                "reasoning_content": "先想清楚再决定调用 %s。" % name,
                "tool_calls": [{
                    "id": "call_%s" % name,
                    "type": "function",
                    "function": {"name": name, "arguments": json.dumps(arguments)},
                }],
            }
        }]
    }


def final_answer():
    return {
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "AGENT-E2E-完成：已查看硬件，继电器动作已执行，命令已运行。",
                "reasoning_content": "收尾。",
            }
        }]
    }


def protocol_error(request):
    """像真机一样校验「有状态协议」，返回错误消息或 None（2026-09-21 审计 AG-1/AG-7）。

    存在的意义与 schema_error 相同：夹具比真机宽松，缺陷就会一路溜到真机才暴露。
    此前这里只校验 schema，于是下面三类「真机必然 400」的报文在离线守门里全绿：

      ① 请求体不是合法 UTF-8 —— 历史里任何一条消息带上非法字节（run_command 输出
         二进制、非 UTF-8 日志、脚本首注释、结构脚本头）都会命中。真网关 400 之后
         那条消息已落在不可回滚的历史里，会话从此每次都以同一字节偏移失败。
         此前 `except ValueError` 顺手把 UnicodeDecodeError 也吞了 → 请求被判成 {}
         后照常 200，**结构上不可能报出这个失败**。
      ② reasoning_content 有状态要求 —— 请求里一旦出现该字段（DeepSeek 思考模式），
         每条带 tool_calls 的历史 assistant 都必须带上它，缺一条真机直接 400。
         用「请求中出现过」作开关而不是无条件要求：OpenAI 兼容驱动压根不认识这个
         字段，无条件要求会把合法请求判死。
      ③ tool_call_id 必须与 assistant.tool_calls 一一配对 —— 孤儿 tool 消息、
         重复回传、或漏回传某个 tool_call，真机都是 400。
    """
    messages = request.get("messages")
    if not isinstance(messages, list):
        return "messages must be an array"

    # ① 非法 UTF-8 由调用方在解码阶段拦下（这里只兜「已解码但含代理项」的极端情况）
    for i, m in enumerate(messages):
        if not isinstance(m, dict):
            return "messages[%d] must be an object" % i

    # ② reasoning_content 有状态要求
    uses_reasoning = any(isinstance(m.get("reasoning_content"), str) for m in messages)
    if uses_reasoning:
        for i, m in enumerate(messages):
            if m.get("role") != "assistant":
                continue
            if m.get("tool_calls") and not isinstance(m.get("reasoning_content"), str):
                return ("messages[%d]: assistant message with tool_calls is missing "
                        "reasoning_content (DeepSeek requires it in thinking mode)" % i)

    # ③ tool_call_id 配对
    declared = []          # assistant 声明的 tool_call id，按出现顺序
    for m in messages:
        if m.get("role") != "assistant":
            continue
        tcs = m.get("tool_calls")
        if tcs is None:
            continue
        if not isinstance(tcs, list):
            return "assistant.tool_calls must be an array"
        for tc in tcs:
            if not isinstance(tc, dict) or not isinstance(tc.get("id"), str):
                return "assistant.tool_calls[].id must be a string"
            declared.append(tc["id"])

    answered = []
    for i, m in enumerate(messages):
        if m.get("role") != "tool":
            continue
        tcid = m.get("tool_call_id")
        if not isinstance(tcid, str):
            return "messages[%d]: tool message requires a string tool_call_id" % i
        if tcid not in declared:
            return ("messages[%d]: tool_call_id %r does not match any assistant "
                    "tool_call" % (i, tcid))
        if tcid in answered:
            return "messages[%d]: tool_call_id %r answered more than once" % (i, tcid)
        answered.append(tcid)
    missing = [c for c in declared if c not in answered]
    if missing:
        return "assistant.tool_calls %r got no matching tool message" % (missing,)
    return None


def schema_error(request):
    """像 DeepSeek 一样严格校验工具 schema，返回错误消息或 None。

    真实 API 会拒绝不合法的 schema（照抄它的原话：
    `Invalid schema for function 'x': {} is not of type "array"`）。
    夹具若不校验，这类 bug 就会一路溜到真机才暴露——本函数存在的意义就是
    把真机的校验前移到离线守门里。
    """
    for t in request.get("tools", []):
        fn = t.get("function") or {}
        name = fn.get("name")
        params = fn.get("parameters")
        if not isinstance(params, dict):
            return "Invalid schema for function '%s': parameters must be an object" % name
        if params.get("type") != "object":
            return "Invalid schema for function '%s': type must be \"object\"" % name
        req = params.get("required")
        if req is not None and not isinstance(req, list):
            return ('Invalid schema for function \'%s\': %s is not of type "array"'
                    % (name, json.dumps(req)))
        props = params.get("properties")
        if props is not None and not isinstance(props, dict):
            return ('Invalid schema for function \'%s\': properties is not of type "object"'
                    % name)
    return None


class Handler(BaseHTTPRequestHandler):
    def _reply(self, payload):
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass	# 客户端超时先走了：本服务是守门夹具，不必抱怨

    def _error_400(self, message):
        data = json.dumps({"error": {"message": message, "type": "invalid_request_error"}},
                          ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(400)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _error_status(self, code, message, retry_after=None, kind="server_error"):
        """带**非 200 状态码**的错误响应（SAN-3 的限流/5xx 场景用）。

        与 _error_400 分开的理由：400 是"请求错了，别再试"，而 429/503 是
        "稍后再试"——宿主的重试判据必须能区分这两类，夹具因此也要能产出两类。
        retry_after 非空时带上 Retry-After 响应头（SAN-3 要验证宿主读得到它）。"""
        data = json.dumps({"error": {"message": message, "type": kind}},
                          ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            if retry_after is not None:
                self.send_header("Retry-After", str(retry_after))
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n) if n > 0 else b"{}"
        if CAPTURE:
            try:
                with open(CAPTURE, "wb") as f:
                    f.write(raw)
            except OSError:
                pass
        if HEADERS:
            try:
                with open(HEADERS, "w") as f:
                    for k, v in self.headers.items():
                        f.write("%s: %s\n" % (k, v))
            except OSError:
                pass
        if MODE == "slow":
            time.sleep(5)
            self._reply({"choices": [{"message": {"role": "assistant", "content": "慢响应"}}]})
            return
        # 先做**严格 UTF-8 解码**（2026-09-21 审计 AG-1）。此前的 `except ValueError`
        # 把 UnicodeDecodeError（ValueError 的子类）一并吞掉，非法字节的请求被当成
        # `{}` 后照常 200——假网关于是比真网关宽松，AG-1 这类字节级缺陷在离线守门里
        # 结构上不可见。现在像真网关一样：解不出来就是 400，并点名首个坏字节位置。
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as e:
            self._error_400("invalid utf-8 in request body: %s (byte 0x%02x at offset %d)"
                            % (e.reason, raw[e.start], e.start))
            return
        try:
            request = json.loads(text)
        except ValueError as e:
            self._error_400("invalid JSON in request body: %s" % e)
            return
        for bad in (schema_error(request), protocol_error(request)):
            if bad is not None:
                self._error_400(bad)
                return
        tool_results = sum(1 for m in request.get("messages", [])
                           if m.get("role") == "tool")

        # ---- SAN-1：单轮返回 N 条工具调用（触发宿主的单轮上限截断分支）----
        # 缺省剧本每轮只返回 1 条调用，而 max_calls_per_round 缺省 16
        # —— 截断分支在 E2E 里从未执行过，因此 SAN-1 那个"截断通知复用
        # tool_call_id、会话被永久打成 400"的缺陷在守门里结构上不可见。
        # 本模式让夹具能产出"一轮多调用"的形态，配合 max_calls_per_round=2 触发截断。
        if MODE.startswith("manycalls"):
            want = int(MODE.split(":", 1)[1]) if ":" in MODE else 3
            if tool_results == 0:
                self._reply({"choices": [{"message": {
                    "role": "assistant", "content": "本轮要查几件事。",
                    "tool_calls": [{
                        "id": "c%d" % i, "type": "function",
                        "function": {"name": "list_hardware", "arguments": "{}"},
                    } for i in range(1, want + 1)]}}]})
            else:
                self._reply(final_answer())
            return

        # ---- SAN-3：限流 / 5xx（非 200 状态码，与 400 的语义刻意不同）----
        # 429x2ok：前两次 429（带 Retry-After: 1），第三次给正常回答。
        #   这是"重试是否真的发生"的决定性判据：不重试则服务端只收到 1 个 POST
        #   且任务失败；重试则收到 3 个 POST 且任务成功。
        # http-503：恒 503（带 Retry-After: 1），用来断言"重试用尽后如实失败"。
        if MODE in ("429x2ok", "http-503"):
            ATTEMPTS["n"] += 1
            if MODE == "429x2ok" and ATTEMPTS["n"] > 2:
                self._reply(final_answer())
                return
            self._error_status(
                429 if MODE == "429x2ok" else 503,
                "rate limited (fake gateway, %s)" % MODE,
                retry_after=1,
                kind="rate_limit_error" if MODE == "429x2ok" else "server_error")
            return

        if MODE == "camera":
            if tool_results == 0:
                self._reply(tool_call("capture_image", {}))
            else:
                self._reply(final_answer())
            return
        if tool_results == 0:
            self._reply(tool_call("list_hardware", {}))
        elif tool_results == 1:
            self._reply(tool_call("run_action_script", {"script": "relay-test.mt"}))
        elif tool_results == 2:
            self._reply(tool_call("run_command", {"command": "echo agent-echo-ok"}))
        else:
            self._reply(final_answer())

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
