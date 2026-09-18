#!/usr/bin/env python3
"""离线假 OpenAI 兼容服务（仅用于 agent 模式守门，不参与构建产物）。

按「已经回传了几条 tool 消息」推进剧本，用来验证 agent 工具循环真的迭代了、
且工具结果真的回传给了模型：
    0 条 → 调 list_hardware
    1 条 → 调 run_action_script(relay-test.mt)
    2 条 → 调 run_command(echo agent-echo-ok)
    ≥3 条 → 给出最终回答

用法: python3 scripts/fake_llm_server.py <port>
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def tool_call(name, arguments):
    return {
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "调用工具 %s。" % name,
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
            }
        }]
    }


class Handler(BaseHTTPRequestHandler):
    def _reply(self, payload):
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n) if n > 0 else b"{}"
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
    HTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
