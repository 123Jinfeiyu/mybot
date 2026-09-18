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
        try:
            request = json.loads(raw)
        except ValueError:
            request = {}
        tool_results = sum(1 for m in request.get("messages", [])
                           if m.get("role") == "tool")
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
