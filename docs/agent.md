# agent 模式（--agent）

> **一句话**：管道模式是「给它一份剧本，它照着演一遍就下台」；agent 模式是
> 「**让它一直站在台上**，由大模型看着可用剧本，一步一步决定下一段演什么」。
> 而**模型调用本身是一颗 Lua 驱动**——和舵机、传感器平级，不引入任何链接期依赖。

## 与管道模式的差别

| | 管道模式（默认） | agent 模式（`--agent`） |
| --- | --- | --- |
| 主流程 | 读一段 `.mt` → 逐行重放 → 退出 | 大模型工具循环，多轮决策后才退出 |
| 硬件生命周期 | `active()` → `run()` → `deactivate()`，一次性 | `active()` → **常驻** → 会话结束才 `deactivate()` |
| 动作脚本数量 | 一次一段（可用 mtmerge 合成） | 任意多段，逐个由智能体挑选执行 |
| 输入源 | `--tcp` / `--input` / stdin | `--task` 一次性，或 stdin 交互 REPL |
| 决策者 | 写脚本的人 | 大模型（受提示词与工具边界约束） |

两种模式**互斥**：`--agent` 与 `--tcp`/`--input` 同时给会被显式拒绝（退出码 2）。

## 快速开始

无硬件自检链路（推荐第一条命令）：

```bash
export BOT4CJ_LLM_KEY=sk-xxx
./build/mybot --robot=robots/relay-test.sql --agent --task "让继电器动作一次"
```

换 DeepSeek 协议（同样的机器人与动作脚本，只换协议驱动）：

```bash
export DEEPSEEK_API_KEY=sk-xxx
./build/mybot --robot=robots/spider1.sql --agent --llm-driver LlmDeepSeek
mybot> 站起来
mybot> 先前走两步，然后向左转
mybot> /exit
```

REPL 内置命令：`/hardware` 打印硬件清单，`/scripts` 打印动作脚本清单，`/exit`（或 `/quit`、Ctrl-D）退出。
Ctrl-C 走正常清理链：当前轮中断、机器人 `deactivate()` 去初始化后进程才退出。

## 命令行参数

| 参数 | 说明 | 缺省 |
| --- | --- | --- |
| `--agent` | 开启 agent 模式 | 关 |
| `--task <文本>` | 一次性任务，跑完即退出；不给则进交互 REPL | — |
| `--llm-config <路径>` | **配置文件**（JSON）：接入点、密钥、模型、采样与 agent 参数 | 见下（自动查找） |
| `--llm-driver <表名>` | **协议驱动**表名 = `modules/<表名>.lua` | `LlmOpenAI` |
| `--llm-var <变量名>` | 承载驱动的硬件变量名 | `llm0` |
| `--llm-endpoint <URL>` | 接入点（覆盖配置文件） | 配置文件 / 驱动缺省 |
| `--llm-model <名称>` | 模型名（覆盖配置文件） | 配置文件 / 驱动缺省 |
| `--llm-key <KEY>` | API key（覆盖配置文件） | 配置文件 / 环境变量，见下 |
| `--llm-max-turns <N>` | 单次任务的最大模型轮数（1-100） | 8 |
| `--llm-max-tokens <N>` | 单次生成最大 token 数 | 4096 |
| `--llm-timeout <秒>` | 单次模型调用超时 | 180 |
| `--scripts-dir <目录>` | 动作脚本根目录，也是智能体可执行路径的**沙箱边界** | `scripts` |
| `--agent-no-shell` | **不**注册 `run_command`（去掉任意 shell 执行能力） | 关（即默认提供） |

取值优先级一律是：**命令行 > 环境变量 > 配置文件 > 驱动内置缺省**。

`--task` / `--llm-*` / `--scripts-dir` / `--agent-no-shell` 出现在**非** agent 模式下会被拒绝（退出码 2），不会静默忽略。

## 配置文件（推荐）

目标 URL、API key、模型等信息放在一份 JSON 配置里，命令行只留 `--llm-config`：

```json
{
  "driver": "LlmOpenAI",
  "endpoint": "https://moma.cmecloud.cn/v1/chat/completions",
  "model": "zhipu/glm-5.3-flash",
  "api_key": "sk-xxx",

  "timeout": 180,
  "max_tokens": 4096,
  "temperature": 0.7,
  "top_p": 0.9,
  "reasoning_effort": "",
  "thinking": "",

  "max_turns": 8,
  "scripts_dir": "scripts",
  "allow_shell": true
}
```

```bash
./build/mybot --robot=robots/spider1.sql --agent            # 自动查找配置文件
./build/mybot --robot=robots/spider1.sql --agent --llm-config /etc/mybot/llm.json
```

- **查找顺序**：`--llm-config` → 环境变量 `BOT4CJ_LLM_CONFIG` → `./mybot.llm.json` → `./mybot.llm.conf.json` → `$HOME/.config/mybot/llm.json`。都没有也能跑（走命令行/环境变量）。
  会话开始时会把实际使用的配置文件打印到 stderr，不会「偷偷」用了哪一份。
- **显式关闭**：`--llm-config none`（或 `BOT4CJ_LLM_CONFIG=none`）= 本次不读任何配置文件。
  缺省查找依赖 CWD，同一份命令换台机器可能读到不同配置——CI/守门/复现问题时应显式关掉或显式指定。
- **字段**：`driver` `var` `endpoint` `model` `api_key` `api_key_file` `timeout` `max_tokens`
  `temperature` `top_p` `reasoning_effort` `thinking` `max_turns` `scripts_dir` `allow_shell`。
  全部可选；空串按「未配置」处理（方便留空占位）。
- **严格解析**：不是合法 JSON、字段类型/范围不符、**出现未知字段**一律拒绝并退出 2。
  拼错一个键（如 `apikey`）而静默走缺省，会让人以为配置生效了——与「未知 `--key` 退出 2」同一口径。
- **密钥可以放在别处**：`api_key_file` 指向一个只存密钥的文件（末尾换行会自动剥掉）。
  若该文件或内联了 `api_key` 的配置文件对同组/其他用户可读，启动时会打印 `chmod 600` 提醒（只提醒，不阻断）。
- **别提交密钥**：仓库 `.gitignore` 已忽略 `mybot.llm.json` / `*.llm.json`；
  模板见仓库根的 `mybot.llm.json.example`。
- **驱动特有字段**（如 DeepSeek 的 `thinking` / `reasoning_effort`）由宿主原样透传给驱动，
  宿主不理解其语义——协议差异仍然只属于协议驱动。

为什么是 JSON 而不是 Lua：配置文件是**数据**，不是代码。Lua 配置要由宿主内嵌状态机去执行，
等于让一份常被复制/分发的文件获得任意代码执行能力；而仓库已有严格 JSON 解析器，复用它零成本。



## 模型调用：一颗 Lua 协议驱动

```
agentloop（C++）        工具循环
  └─ agentproto（C++）  规范化消息/工具 ⇄ Json
       └─ agentdriver（C++）  ChatTransport：把请求交给驱动变量、取回响应
            └─ llm0 = modules/LlmOpenAI.lua      ← 普通硬件变量
                 └─ lib/llm_wire_openai.lua      ← 协议整形（provider 报文）
                      └─ lib/llm_http.lua        ← ffi 直驱 libcurl（HTTPS/TLS）
```

**为什么不把 HTTP 写进宿主**：

- 换模型/换厂商 = **换一个 Lua 文件**，宿主不重编、不链接任何 HTTP 库；
- 项目「零第三方 C++ 依赖（仅 LuaJIT + pthread + dl）」的承诺在这条路径上依然成立；
- 驱动生态的一切照旧适用：`build/call/destroy` 契约、`guard_build` 回滚、`docs/drivers.md`、
  `test/test_*.lua` 离线用例；模型驱动也能被 `.mt` 动作脚本或 `@Daemon` 调用；
- 协议差异（如 DeepSeek 的思考模式）**只能**由驱动承担——它有状态，宿主一无所知。

### 规范化协议（宿主 ⇄ 驱动）

宿主传进 `call()` 的参数：

```lua
{ model=, endpoint=, timeout=, max_tokens=, temperature=, top_p=,
  messages = { {role="system"|"user", content=},
               {role="assistant", content=, tool_calls={{id=,name=,arguments=}}, extras={}},
               {role="tool", tool_call_id=, content=} },
  tools = { {name=, description=, parameters={...}} } }
```

驱动返回：

```lua
{ result="OK", content=, tool_calls={{id=,name=,arguments=}}, extras={} }
{ result="ERR", message="人话报错" }
```

`extras` 是 provider 特有字段的透明通道（如 DeepSeek 的 `reasoning_content`）：
宿主不理解、不修改，只随 assistant 消息逐轮原样带回，由驱动决定怎么用。
API key 不在这个表里——驱动自己读环境变量。

> **写驱动时注意「空数组」**：宿主把 Json 转成 Lua 表、驱动再编码回 JSON，而 Lua 的
> `{}` 无法区分空数组与空对象——`[]` 会变成 `{}`。JSON Schema 的 `required` 必须是
> 数组，DeepSeek 会严格校验并直接 400：
> `Invalid schema for function 'x': {} is not of type "array"`（实测踩过）。
> `modules/lib/json.lua` 因此提供显式哨兵 `json.empty_array`，两个内置驱动在 schema
> 语义已知处把 `required` 规范化成它。自写驱动若产出结构里含空数组，记得用哨兵。
> 假模型服务 `scripts/fake_llm_server.py` 会像 DeepSeek 一样校验工具 schema，
> 这类问题在离线守门即可暴露。

### 内置两个协议驱动

| 驱动 | 接入点（缺省） | 模型（缺省） | 特点 |
| --- | --- | --- | --- |
| `LlmOpenAI` | `https://moma.cmecloud.cn/v1/chat/completions` | `zhipu/glm-5.3-flash` | OpenAI 兼容：「当前协议」 |
| `LlmDeepSeek` | `https://api.deepseek.com/chat/completions` | `deepseek-chat` | DeepSeek：思考模式 + `reasoning_content` |

两者都是 OpenAI 报文骨架，但 DeepSeek 有三处**实质差异**，由 `lib/llm_wire_deepseek.lua` 承担：

1. **思考模式开关**：请求体带 `{"thinking":{"type":"enabled"}}`，强度用 `reasoning_effort`
   （可经参数或环境变量 `BOT4CJ_LLM_REASONING_EFFORT` 给；传 `thinking="disabled"` 关闭）。
2. **`reasoning_content` 回传**：与 `content` 同级返回。**请求带 `tools` 时，历史轮次的
   `reasoning_content` 必须完整回传，否则 API 返回 400**——这是有状态要求，
   宿主靠 `extras` 逐轮带回、驱动负责回填，`scripts/agent_e2e.sh` 有专门断言。
3. **采样参数限制**：思考模式不支持 `temperature`（传了也被静默忽略），`top_p` 生效但
   下限被抬到 0.95；非思考模式 `top_p` 恒为 1.0。驱动按模式决定发什么，
   避免「发了一个看起来生效、其实被忽略」的参数。

### 结构脚本声明 vs 自动注入

优先用**结构脚本声明**（结构脚本是唯一事实源）：

```sql
CREATE TABLE LlmDeepSeek (name String);
INSERT INTO LlmDeepSeek (name) VALUES ('llm0');
```

没声明时宿主按 `--llm-driver`/`--llm-var` **自动注入**一个——所以任何既有
`robots/*.sql` 都能直接 `--agent` 跑起来，切换协议也只需换一个命令行选项。
表名与变量名都会校验为安全标识符（它们会被拼进合成 SQL，且表名还会当
`modules/<表名>.lua` 解析）。

### 自己写第三个驱动

照 `modules/LlmDeepSeek.lua` 抄一份：`build` 里调 `llm_http.load()` 做环境自检、
`call` 里把规范化请求整形后交给 `llm_wire_openai.lua` 的通用实现，或完全自己实现。
`result`/`message`/`mode` 三个键名由框架保留。

## 给智能体的工具

| 工具 | 作用 | 关键约束 |
| --- | --- | --- |
| `list_action_scripts` | 列出 `--scripts-dir` 下全部 `.mt`（相对路径 + 首行说明） | 只读 |
| `run_action_script` | 在**常驻**机器人上执行指定 `.mt` | 只允许 `--scripts-dir` 下的相对路径；拒绝绝对路径、`..`、非 `.mt`；单文件 8 MiB 上限 |
| `list_hardware` | 列出结构脚本注册的硬件变量（名字 + 种类） | 只读清单；**不**调用 `read()`/`play()`，绝无副作用 |
| `run_command` | 在主机上执行 shell 命令（退出码 + stdout + stderr） | 超时（缺省 10s，上限 60s，超时整进程组 SIGKILL）、单流 64KB 截断、环境变量清洗；`--agent-no-shell` 可关闭 |

`run_action_script` 执行失败（脚本名错、硬件未接、解析错误）不会中断会话：报错文本作为工具结果回传模型，由模型决定换个做法还是如实汇报。

## 安全说明（请务必读）

1. **`run_command` 是无白名单、无审批的任意 shell 执行**。模型可能被外部输入（文件内容、设备读数、网络数据）诱导执行任意命令。
   仅在可信环境（你自己的机器人主机）使用；需要对外暴露，请先加命令审批或在进程外隔离（专用用户 / 容器 / 网络命名空间）。
   不需要 shell 能力时加 `--agent-no-shell`。
2. **子进程环境已清洗**：名称含 `key` / `token` / `secret` / `password` / `passwd` / `credential` 的变量不会传给子进程——
   这天然覆盖 `BOT4CJ_LLM_KEY`，模型无法用 `run_command` 读走自己的 API key。
3. **API key 不进驱动参数表、不进调试转储**：密钥可由配置文件、`--llm-key` 或环境变量提供，
   但宿主只会把它注入本进程环境变量 `BOT4CJ_LLM_KEY`（真正读它的是 Lua 驱动）——
   驱动的参数表与 `BOT4CJ_DEBUG` 的参数转储里都不会出现它。
   环境变量链：`BOT4CJ_LLM_KEY` → `ROBOT_AGENT_API_KEY` → `DEEPSEEK_API_KEY`（配置文件里也能写 `api_key`/`api_key_file`）。
   落盘的密钥请 `chmod 600`：启动时对「同组/其他用户可读」会打印提醒；仓库 `.gitignore` 已挡住 `mybot.llm.json` 一类文件名。
   另外 `--llm-key` 会出现在 `/proc/<pid>/cmdline`（本机所有用户可读），能用配置文件或环境变量就别用它。
4. **动作脚本路径是沙箱边界**：`run_action_script` 只接受 `--scripts-dir` 下的 `.mt` 相对路径。这是防误用护栏，
   不是对抗恶意模型的边界——真正的边界仍然是「模型输出即不可信输入」这条工程常识。
5. **仪表器件不会被工具误触发**：`list_hardware` 不读值（对舵机等执行器，`play({})` 会真的下发指令）。
6. **驱动获得网络能力**：`modules/Llm*.lua` 会把请求发到配置的接入点。按本项目的威胁模型，
   `modules/**/*.lua` 本就是「任意原生代码执行入口，信任级等同 mybot 用户」（见 [reference.md](reference.md) 的沙箱威胁模型），
   故这不是新增信任假设，但接外部接入点时请自行确认其可信度。
7. **上电时机**：配置错误、缺 key、驱动名非法都在 `active()` 之前报错退出，不会出现「带电空等」。

## 运行期依赖：libcurl

agent 模式**不需要构建期依赖**（`cmake` 探测不到任何东西也不会失败），但运行期需要
`libcurl.so`：`modules/lib/llm_http.lua` 用 LuaJIT ffi 在 `build()` 阶段按
`libcurl.so.4` → `libcurl-gnutls.so.4` → `libcurl.so` 顺序加载。缺它时驱动 build 返回
清晰 ERR（提示 `apt install libcurl4`），机器人不会上电。

```bash
sudo apt install libcurl4     # Debian/Ubuntu/Raspberry Pi OS 通常已随 curl 装好
```

HTTPS/TLS 由 libcurl 负责（默认 CA 路径），驱动不碰 OpenSSL，也不需要开发包头文件。

## 源码分层

| 文件 | 职责 | 依赖 |
| --- | --- | --- |
| `src/jsonparse.{hpp,cpp}` | 严格 JSON 文本解析（宿主侧） | 无 |
| `src/llmconfig.{hpp,cpp}` | 配置文件读取（JSON、严格校验、缺省查找、密钥文件与权限体检） | 无 |
| `src/agentproto.{hpp,cpp}` | 规范化消息/工具 ⇄ Json | 无 |
| `src/agenttools.{hpp,cpp}` | 工具注册表与四个工具、脚本路径沙箱、硬件描述 | 无 |
| `src/agentloop.{hpp,cpp}` | 工具循环（历史、轮次上限、回滚、extras 透传） | 无 |
| `src/agentmode.{hpp,cpp}` | CLI 配置组装、系统提示词、一次性/REPL 外壳 | 无 |
| `src/agentdriver.{hpp,cpp}` | `ChatTransport` 适配：调驱动的 `playQuiet()` | 无 |
| `modules/lib/json.lua` | Lua 侧严格 JSON 编解码 | 无 |
| `modules/lib/llm_http.lua` | ffi 直驱 libcurl 的 HTTP 传输 | 运行期 libcurl |
| `modules/lib/llm_wire_openai.lua` | OpenAI 兼容报文整形与响应映射 | 无 |
| `modules/lib/llm_wire_deepseek.lua` | DeepSeek 差异（thinking / reasoning_content / 采样参数） | 无 |
| `modules/LlmOpenAI.lua`、`modules/LlmDeepSeek.lua` | 协议驱动（表名 = 文件名） | 无 |

宿主侧**没有任何一处**知道 provider 报文字段。

## 测试

```bash
bash scripts/agent_e2e.sh        # 离线端到端：假模型服务 + 真实工具循环（四场景）
luajit test/test_json_lua.lua    # Lua JSON 编解码
luajit test/test_llm_drivers.lua # 两个协议的整形规则（不碰网络）
./build/mybot_tests agent        # C++：proto / driver / tools / loop / config
```

- `scripts/agent_e2e.sh` 用本地假模型服务跑四个场景：默认驱动完整工具循环（并抓报文断言
  OpenAI 形态）、`--llm-driver LlmDeepSeek`（断言 thinking / 不发 temperature /
  `reasoning_content` 回传）、`--llm-timeout` 生效（防 curl 变参 cast 退化）、
  `--llm-config` 配置文件（URL/KEY/MODEL 全部来自文件，且**环境里不放任何密钥**）。
- 全量守门 `bash test/run_all.sh` 已包含上述全部；缺 python3 或 libcurl 时按 **SKIP**
  计入「未执行清单」（可见，不假绿）。

## 已知限制

- **非流式**：`stream=false`，长回答要等整轮生成完；接口超时用 `--llm-timeout` 调整。
- REPL 历史无自动裁剪，长会话上下文持续增长。
- 无记忆持久化（重启即忘）。
- 只实现了两个 OpenAI 兼容协议驱动；接非 OpenAI 形态的厂商（如 Anthropic）需要新写一个驱动
  （协议整形全在 Lua 侧，宿主不用改）。
- 驱动参数表的嵌套深度受 `kMaxLuaNesting`（8 层）约束：工具 schema 目前最深处为 6 层，留有余量但不宜再加嵌套。
