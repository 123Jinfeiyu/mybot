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
模型调用期间（最长 600s）不可中断，此时**再按一次 Ctrl-C 即立即强杀**——跳过清理，
执行器可能停在最后指令位置，属"停不下来"时的逃生口（详见 [reference.md](reference.md)）。

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
| `--llm-max-turns <N>` | 单次任务的最大模型轮数（1-100） | 8 |
| `--llm-max-tokens <N>` | 单次生成最大 token 数（1-200000） | 4096 |
| `--llm-timeout <秒>` | 单次模型调用超时，范围 1-600 | 180 |
| `--scripts-dir <目录>` | 动作脚本根目录，也是智能体可执行路径的**沙箱边界** | `scripts` |
| `--prompt-file <路径>` | **提示词包**，可重复传入（文件或目录），拼接后追加到系统提示词（见下节） | — |
| `--agent-shell` | **显式开启** `run_command`（任意 shell 执行） | **不提供**（见「安全」节） |
| `--camera-var <变量名>` | 承载摄像头驱动的变量名 | `cam0` |
| `--camera-device <路径>` | 摄像头设备（需支持 MJPEG；驱动只放行 `/dev/videoN`） | `/dev/video0` |
| `--camera-src <图片>` | 模拟图片来源：不接摄像头也能跑通「拍照 → 进上下文」（只读该文件所在目录内的文件） | — |
| `--no-camera` | **不**注册 `capture_image`（去掉拍照能力） | 关（默认注册，前提是该变量解析到 Lua 驱动，见安全说明第 5 条） |

取值优先级一律是：**命令行 > 环境变量 > 配置文件 > 驱动内置缺省**。
API key 没有命令行入口（只从环境变量或配置文件取）：argv 对同用户进程可读（`/proc/<pid>/cmdline`）。

`--task` / `--llm-*` / `--scripts-dir` / `--agent-shell` / `--prompt-file` / `--camera-var` / `--camera-device` / `--camera-src` / `--no-camera` 出现在**非** agent 模式下会被拒绝（退出码 2），不会静默忽略。

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
  "allow_shell": false,
  "prompt_files": ["prompts/spider_robot"]
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
  `temperature` `top_p` `reasoning_effort` `thinking` `max_turns` `scripts_dir` `allow_shell`
  `prompt_files` `camera_var` `camera_device` `camera_src` `allow_camera`。
  全部可选；空串按「未配置」处理（方便留空占位）；`prompt_files` 为字符串数组
  （每项为文件或目录，语义同 `--prompt-file`），空数组按「未配置」。
  `allow_shell` 是**只能关不能开**的字段：写 `false`（或省略）无副作用，写 `true`
  会被拒绝加载并退出 2——开启 `run_command` 只能用命令行 `--agent-shell`
  （理由见 [安全说明](#安全说明请务必读) 第 1 条）。
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

> **内置缺省端点是特定第三方网关**：CLI / 配置文件 / 环境链都没配 endpoint
> 时，报文会发往上表缺省 URL。每次进程首次这样回退时驱动会向 stderr 注记
> 一行（`[llm-wire] 未配置 endpoint，使用驱动内置缺省 …`），便于发现「什么
> 都没配就跑起来」的会话；正式使用建议在配置文件里显式指定 endpoint。

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
| `list_action_scripts` | 列出 `--scripts-dir` 下全部 `.mt`（相对路径 + 首行说明）；符号链接条目不列 | 只读 |
| `run_action_script` | 在**常驻**机器人上执行指定 `.mt` | 只允许 `--scripts-dir` 下的相对路径；拒绝绝对路径、`..`、非 `.mt`、**符号链接**；单文件 8 MiB 上限 |
| `list_hardware` | 列出结构脚本注册的硬件变量（名字 + 种类） | 只读清单；**不**调用 `read()`/`play()`，绝无副作用 |
| `capture_image` | **拍一张照片并把画面注入上下文**，多模态模型因而能「看见」环境 | 经 `modules/Camera.lua` 取帧（V4L2 + MJPEG）；`--no-camera` 可关闭；图片走独立通道（见下节） |
| `run_command` | 在主机上执行 shell 命令（退出码 + stdout + stderr） | 超时（缺省 10s，上限 60s，超时整进程组 SIGKILL）、单流 64KB 截断、环境变量清洗、继承 fd 全部 CLOEXEC；**缺省不注册**，须命令行 `--agent-shell` 显式开启（配置文件的 `allow_shell` 只能关不能开，写 `true` 会拒绝加载） |

`run_action_script` 执行失败（脚本名错、硬件未接、解析错误）不会中断会话：报错文本作为工具结果回传模型，由模型决定换个做法还是如实汇报。

## 摄像头：capture_image

摄像头也是一颗 Lua 驱动（`modules/Camera.lua`）：V4L2 mmap 取帧、直接拿摄像头硬件编码好的
**MJPEG**，因此宿主不必链接任何采集/编码库（robot_agent 的 C++ 版为此需要 libjpeg）。
未声明时宿主会按 `--camera-var` 自动注入（与 LLM 驱动同一注入机制）。

```bash
# 无摄像头也能跑通全链路：用一张图片当"摄像头"
./build/mybot --robot=robots/relay-test.sql --agent --camera-src /tmp/pic.jpg \
  --task "拍张照片看看周围有什么"

# 真实摄像头（缺省 /dev/video0）
./build/mybot --robot=robots/spider1.sql --agent --task "看看前方有没有障碍物"
```

### 图片怎么进上下文

OpenAI 协议的 **tool 消息只允许文本**，所以图片不能塞进工具结果里。做法是：
工具循环先把本轮所有 `tool` 消息回传完（协议要求它们紧跟 assistant 的 `tool_calls`），
**再**追加一条带图片的 `user` 消息：

```jsonc
// 规范化消息（宿主 → 驱动）：宿主**直接产出 content 数组**，没有 images 键
{ "role": "user", "content": [
    { "type": "text", "text": "以上工具返回的图片如下，请结合画面内容继续。" },
    { "type": "image_url", "image_url": { "url": "data:image/jpeg;base64,..." } } ] }
// 驱动把 content 原样透传给 provider（OpenAI 兼容形态本就如此；换厂商时由驱动
// 把它映射成自己协议的形态，例如 Anthropic 的 image/source 块）
```

`content` 数组是**宿主**组装的（`src/agentproto.cpp` 的 `messageToJson`，图片部分
在文本之后），驱动不参与展开——写第三方协议驱动时照上面的形态**透传或映射**
`content` 即可，不要去等一个并不存在的 `images` 字段。

图片与文字一样留在会话历史里，REPL 的后续任务仍然看得到它。

data URL 的 MIME 取自**驱动返回的 `format` 字段**（`image/jpeg`、`image/png`、
`image/gif`、`image/webp`）：驱动按**魔数**判定格式，宿主只做一次「声明与内容
是否自洽」的交叉校验，不符即拒绝发出（否则会把错标签发给网关，那边只会回一个
含混的「图片解析错误」）。所以第三方摄像头驱动必须**如实返回 `format`**，
且只支持这四种；仿真来源（`--camera-src`）不是这四种时驱动会在 `build` 阶段
就以 `unsupported sim image format` 拒绝。

> **需要多模态模型**。`capture_image` 只负责把画面送到；模型能不能看图取决于你选的
> 模型/接入点。DeepSeek 的 `deepseek-chat` 不吃图片输入——它可能忽略图片或直接报错，
> 换一个支持视觉的模型即可（本项目对协议不做假设，两个内置驱动都如实转发图片）。

### 设计要点

- **摄像头是可选外设**：**真机路径下**驱动 `build` 不打开设备、也不返回 ERR，否则一台
  没接摄像头的机器人会连电都上不了。设备在第一次拍照时按调用参数惰性打开，打不开就如实
  报给智能体（「无法打开摄像头 /dev/video0（errno=2）…」），模型据此自己决定改做什么。
  例外：显式给了仿真来源（结构脚本里声明 `src` 元数据列，或 `--camera-src`）时，
  路径越界 / 文件缺失 / 格式不受支持会**刻意在 `build` 阶段返回 ERR**——拒绝一次注定
  失败的仿真，比上电后再报错更早、更清楚。agent 模式的自动注入路径不受影响（宿主只在
  调用时才传 `src`）。
- **热插拔**：抓拍失败即释放设备，下次调用重试打开；换设备也靠同一句重开。
- **首帧丢弃**：起流后的第一帧曝光/白平衡常常未稳，丢弃后再取一帧（与 robot_agent 同做法）。
- **截断帧重取**：UVC 设备可能吐出熵编码中途截断的 MJPEG 帧（2026-09-19 实测某设备约半数帧如此；
  内核不置 `V4L2_BUF_FLAG_ERROR`，宽松解码器照样出图），模型网关的严格解析器却会直接 400
  「图片输入格式/解析错误」。取帧后按「帧尾 EOI」校验（`lib/mjpeg_frame.lua`；只在帧尾窗口搜索，
  防 APP1 内嵌缩略图的 EOI 误判），不完整整帧丢弃重取（上限 8 帧），不让坏帧流进模型上下文。
- **仿真**：`--camera-src <图片>` 直接读文件并 base64 返回，与真机走同一段返回逻辑，
  因此「拍照 → 进上下文 → 模型看图」这条链路在没有摄像头的开发机上也能完整回归。
- **路径白名单**：驱动读文件/开设备都受白名单约束——设备只放行 `/dev/videoN`；
  仿真图片只放行 `--camera-src` 所在目录（或环境变量 `BOT4CJ_CAMERA_SRC_DIR` 列出的
  目录，冒号分隔）**之内**的文件，判定走 `realpath`，符号链接展开后仍越界即拒。
  两者都没给时仿真模式默认禁用。这样 `src`/`path` 就不再是「读任意路径」的原语。
- **ABI 自检**：V4L2 结构尺寸与 ioctl 编号既在驱动加载期 `assert`，又在构建期由
  `test/test_v4l2_consts.c` 与 `<linux/videodev2.h>` 比对（F-24 同族）——ffi 声明比内核
  小几字节就是越界读写，必须让它在构建期就失败。

## 提示词包：绑定机器人身份

内置系统提示词只讲机器人本体（结构、硬件清单、脚本清单、工作方式）；「这个机器人
**是谁**、该怎么行事」由提示词包定义，移植自 robot_agent 的 `--prompt-file` 能力：

```bash
# 目录：收集其中全部 .md，按文件名自然排序（编号即装载顺序）
./build/mybot --robot=robots/spider1.sql --agent --prompt-file prompts/spider_robot/

# 可重复：显式文件保持传入顺序，与目录可混用
./build/mybot --robot=robots/spider1.sql --agent \
	--prompt-file prompts/spider_robot/ --prompt-file my-extra-rules.md
```

- **拼接规则**：每个文件去首尾空白后为空则跳过；只有一份时保持原文不加标题；
  多份之间以空行分隔、每份前加 `## 文件名` 分节。整包**追加在系统提示词内置段落
  之后**，用户输入仍只是 user 消息——markdown 定义「这个机器人怎么做事」，
  任务文本定义「现在做什么」。
- **配置文件等价写法**：`"prompt_files": ["prompts/spider_robot"]`（字符串数组，
  每项为文件或目录）。命令行给过 `--prompt-file` 就整体覆盖配置文件列表，
  与其他选项的「命令行 > 配置文件」口径一致。
- **加载时机**：在 `buildAgentConfig`（配置组装期）加载——路径写错、目录里没有
  `.md` 都在结构装载与上电之前报错退出 2（「配置错误不碰真机」）；单文件 1 MiB
  上限，超限响亮拒绝而非静默截断（截断过的提示词悄悄丢约束，更难查）。
- **示例包**：`prompts/spider_robot/` 定义蜘蛛机器人的身份、摄像头能力与动作脚本用法、
  动作脚本语义表、行为纪律（前方有障碍不得前进），展示「大模型大脑 + 脚本驱动
  硬件」的完整绑定链路。提示词包是纯 Markdown，照着这个目录结构写你自己的即可。

## 安全说明（请务必读）

1. **`run_command` 是无白名单、无审批的任意 shell 执行**。模型可能被外部输入（文件内容、设备读数、网络数据）诱导执行任意命令。
   正因如此**它缺省不注册，且只能用命令行 `--agent-shell` 开启**：配置文件里的 `allow_shell: true`
   会被**拒绝加载**并指名原因（配置只能关不能开）。理由是缺省查找会读 CWD 的 `./mybot.llm.json`——
   同一个未经审查的落地文件既能改 `endpoint`（把含结构脚本、硬件清单、摄像头画面的上下文外发到任意主机），
   又能开 shell（模型回复直接变成主机命令），「这个文件存在」不构成授权。
   开启后请在可信环境（你自己的机器人主机）使用；需要对外暴露，请先加命令审批或在进程外隔离（专用用户 / 容器 / 网络命名空间）。
2. **密钥的环境流转有两道防线**（2026-09-20 审计 A 收口）：
   ① `run_command` 的子进程环境按**整词**清洗——名称含 `key` / `token` / `secret` / `password` /
   `passwd` / `credential` 的变量不传给子进程（`BOT4CJ_LLM_KEY` 天然落在内，`KEYSTONE`/`MONKEY` 不误伤）；
   ② 宿主在注入密钥后**抹掉自己 exec 时环境块里的那些串**——光清洗子进程那份挡不住
   `cat /proc/<宿主 pid>/environ`（同 UID 可读），而常驻 agent 一跑就是几小时。
   **落盘来源仍是明文可读**：密钥若来自配置文件（`api_key` 字段）或密钥文件（`api_key_file`），
   子进程直接 `cat` 就能拿到，`chmod 600` 对同用户无效——启用 `run_command` 时命中这两种来源
   会在启动时打印告警（环境变量来源不告警，因为两道防线都盖住了）。
3. **API key 不进驱动参数表、不进调试转储**：密钥只由配置文件（`api_key`/`api_key_file`）或环境变量提供
   （宿主**不提供**命令行传密钥的选项），宿主把它注入本进程环境变量 `BOT4CJ_LLM_KEY`（真正读它的是 Lua 驱动）——驱动的参数表与 `BOT4CJ_DEBUG` 的参数转储里都不会出现它。
   - 环境变量链：`BOT4CJ_LLM_KEY` → `ROBOT_AGENT_API_KEY` → `DEEPSEEK_API_KEY`（配置文件里也能写 `api_key`/`api_key_file`）
   - **两道环境防线只挡「顺手一读」**：同 UID 的进程只要能 `ptrace` 宿主，就能从内存里把密钥捞出来
     （`/proc/<pid>/mem`）。想同时挡住其他本机用户与 agent 自己的 shell，得把 agent 放进**专用用户 /
     容器 / 网络命名空间**，而不是指望上面那两条。
   - 落盘的密钥请 `chmod 600`，启动时对「同组/其他用户可读」会打印提醒，仓库 `.gitignore` 已挡住 `mybot.llm.json` 一类文件名
   - 曾有的 `--llm-key` 已移除：它必然出现在 `/proc/<pid>/cmdline`（本机所有用户可读）
4. **动作脚本路径是沙箱边界**：`run_action_script` 只接受 `--scripts-dir` 下的 `.mt` 相对路径。这是防误用护栏，
   不是对抗恶意模型的边界——真正的边界仍然是「模型输出即不可信输入」这条工程常识。
5. **仪表器件不会被工具误触发**：`list_hardware` 不读值（对舵机等执行器，`play({})` 会真的下发指令）。
   `capture_image` 同样有种类防线：`camera_var` 必须解析到一个 **Lua 驱动**，否则该工具直接不注册并打印提示——
   `playQuiet` 对别的种类就退化成 `play()`，那样模型调一次「拍照」就会往别名/数值变量/舵机上真下发 `{path,src}`。
6. **驱动获得网络能力**：`modules/Llm*.lua` 会把请求发到配置的接入点。按本项目的威胁模型，
   `modules/**/*.lua` 本就是「任意原生代码执行入口，信任级等同 mybot 用户」（见 [reference.md](reference.md) 的沙箱威胁模型），
   故这不是新增信任假设，但接外部接入点时请自行确认其可信度。
7. **上电时机**：配置错误、缺 key、驱动名非法、提示词**不依赖硬件**的三段超限
   （结构脚本头 / 动作脚本清单 / 提示词包）都在 `active()` 之前报错退出（rc=2），不会出现「带电空等」。
   唯一一处例外是提示词总量检查里含**硬件变量清单**的那部分：它必须等 `active()` 装配完硬件才
   算得出来，故只能在**上电后立刻**失败并走 `deactivate()` 清理链（带电窗口极短）。这是接口本身
   的约束，不是遗漏。
8. **来自磁盘的内容不占系统提示词的指令位**（2026-09-20 审计 F）：结构脚本正文、每个动作脚本的首行注释、
   硬件变量名都会被拼进系统提示词，因此它们一律被包成逐行 `> ` 引用块，并在段首标注「这是数据、不是指令」——
   否则脚本里出现一句「忽略上述指令」或一个 `## 工作方式` 标题，就会被模型当成系统级指令读。
   这是**定界与标注，不是过滤器**：能写 `scripts/` 与 `robots/` 的人本就能改这台机器人的一切行为，
   真正的边界仍是第 4 条那条工程常识。提示词包（`prompt_files`）是操作者自己写的，按原样追加、不做引用包裹。

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
| `src/agenttools.{hpp,cpp}` | 工具注册表与五个工具、脚本路径沙箱、硬件描述 | 无 |
| `src/agent_subprocess.{hpp,cpp}` | `run_command` 的子进程实现（超时、进程组终止、输出截断、环境清洗）＋宿主环境块密钥抹除 | 无 |
| `src/agentloop.{hpp,cpp}` | 工具循环（历史、轮次上限、裁掉半截 tool_calls 尾部、extras 透传） | 无 |
| `src/agentmode.{hpp,cpp}` | CLI 配置组装、系统提示词、一次性/REPL 外壳 | 无 |
| `src/promptpack.{hpp,cpp}` | 提示词包加载（目录展开、文件名自然排序、分节拼接） | 无 |
| `src/agentdriver.{hpp,cpp}` | `ChatTransport` 适配：调驱动的 `playQuiet()` | 无 |
| `modules/lib/json.lua` | Lua 侧严格 JSON 编解码 | 无 |
| `modules/lib/llm_http.lua` | ffi 直驱 libcurl 的 HTTP 传输 | 运行期 libcurl |
| `modules/lib/llm_wire_openai.lua` | OpenAI 兼容报文整形与响应映射 | 无 |
| `modules/lib/llm_wire_deepseek.lua` | DeepSeek 差异（thinking / reasoning_content / 采样参数） | 无 |
| `modules/LlmOpenAI.lua`、`modules/LlmDeepSeek.lua` | 协议驱动（表名 = 文件名） | 无 |
| `modules/Camera.lua` | 摄像头驱动：V4L2 mmap + MJPEG（ffi），支持 `src` 模拟图片 | 无 |

宿主侧**没有任何一处**知道 provider 报文字段。

## 测试

```bash
bash scripts/agent_e2e.sh        # 离线端到端：假模型服务 + 真实工具循环（六场景）
luajit test/test_json_lua.lua    # Lua JSON 编解码
luajit test/test_llm_drivers.lua # 两个协议的整形规则（不碰网络）
luajit test/test_camera.lua      # 摄像头驱动（仿真 + base64 + 错误路径）
# C++：proto / driver / tools / loop / config（协议、驱动、工具、循环、配置、提示词包、CLI 选项面）
# 用 **agent:** 而不是裸 agent——后者是子串匹配，只命中用例名里含 "agent" 的那部分，
# 会漏掉 promptpack / llmconfig / cliargs / agentconfig / agentmode
# （2026-09-21 AG-11 实测 86 vs 108；第三轮 AG-22 又发现 agentconfig_/agentmode_
# 两族 15 例仍不在表里，补齐后为 130 例，并把守门改成**派生式**：注册表里每个
# 用例名前缀要么在作用域表、要么在 test_util.cpp 的「非 agent 面」roster 里带
# 理由登记——漏登记一整族即红，而不是静默少跑）。
./build/mybot_tests agent:
```

「**agent 全部守门**」请用下面这一条（Lua `test_agent*.lua` + agent E2E + C++ 的
`agent:` 作用域 + 4 条 agent CLI 负路径）。裸 `agent` 与 `agent:` **各只覆盖一半
表面**——四类分段要的过滤器形态不同（文件名子串 / `scope:` 语法 / 分段门），
映射单点在 `test/run_all.sh` 顶部并带自证；详见 [testing.md](testing.md)。

```bash
bash test/run_all.sh agent-all
```

- `scripts/agent_e2e.sh` 用本地假模型服务跑六个场景：默认驱动完整工具循环（并抓报文断言
  OpenAI 形态）、`--llm-driver LlmDeepSeek`（断言 thinking / 不发 temperature /
  `reasoning_content` 回传）、`--llm-timeout` 生效（防 curl 变参 cast 退化）、
  `--llm-config` 配置文件（URL/KEY/MODEL 全部来自文件，且**环境里不放任何密钥**）、
  `capture_image` 拍照（断言线上报文里真的出现 `image_url` 多模态内容，且是 data URL 包的真 JPEG）、
  `--prompt-file` 提示词包（断言包内容进入 system 消息、追加在内置段落之后、按文件名自然排序分节）。
- `test/test_camera_v4l2.lua` 是真机抓帧回归，需 `/dev/video*`（文件标记 `-- requires: video`，
  无设备时守门自动跳过）；`test/test_v4l2_consts.c` 在构建期把 V4L2 常量与内核头对齐。
- 全量守门 `bash test/run_all.sh` 已包含上述全部；缺 python3 或 libcurl 时按 **SKIP**
  计入「未执行清单」（可见，不假绿）。

## 已知限制

- **非流式**：`stream=false`，长回答要等整轮生成完；接口超时用 `--llm-timeout` 调整。
- REPL 历史按字节预算自动裁剪（缺省 4 MiB）：超过预算时按「任务块」整块裁掉最老的
  任务（含其中的 base64 图片），system 提示词与最新一个任务无条件保留——更早的任务
  上下文因此不再可见。单块超预算不封顶（正在进行的对话不能裁空），所以单张图另有
  宿主侧硬上限：`capture_image` 拒绝 base64 超过 2 MiB 的图像。该判据有**两道**，
  第二道（base64 长度）只是驱动漏报字节数时的兜底：正常路径先用驱动声明的原始
  字节数判（上限 = 2 MiB × 3/4 = 1572864 字节），**在 base64 编码与跨 Lua 边界
  之前**就拒绝——否则一张注定被拒的图（驱动侧仿真上限 16 MiB）要先被编码、
  跨界搬运才丢掉（2026-09-21 第三轮审计 AG-19 实测：10 MiB 的图让进程峰值
  219 MiB → 修复后 163 MiB，且不再做无用的编码）。
- 失败时只裁掉「半截的 tool_calls 尾部」：**达到轮次上限**这一种失败会原样保留本轮
  历史（那些动作脚本已真跑过、硬件不会倒回去，抹掉等于骗模型说什么都没发生）。
- **任务内预算的三条确切边界**（第三轮审计 AG-21 实测后写明，别按「硬上限」理解）：
  ① `maxTaskToolResultBytes` / `maxTaskImageBytes` 是**任务内累计**上限，但**裁剪
  （maxHistoryBytes）只在每个任务开头做一次**——所以任务进行中历史可以从 2 MiB 长到
  约 6 MiB（文本 2 + 图片 4）才在下一次任务开头被裁，4 MiB 是**稳态**上限而非瞬时上限；
  ② 图片额度逐任务**单向累加**：一旦某张图装不下，本任务后续 `capture_image` 都不会
  成功（降级说明会明说「额度已经用尽……再调用也不会成功」并给出改走文字结论的出路）；
  ③ 降级说明**自己也计入预算**，所以「反复超限」不会往历史里无界灌说明文本。
- **单张图的峰值内存仍是图像体积的数十倍**（第三轮审计 AG-19 实测：786 KB JPEG →
  40 MiB、1.5 MB JPEG → 55 MiB、10 MB（超限被拒）→ 163 MiB）。同一条 payload 在
  「驱动缓冲 → Lua 字符串 → base64 → Json 节点 → data URL → 会话历史 → 请求体」
  上各有副本，其中「用的时候不拷贝」这一半（`Json::getValue()` 的 6 次整串拷贝、
  超限图先编码等）已在 AG-19 收口；剩下的是传输层架构问题（要真正压下来得让图片
  以引用/流式形态穿过这条链），本版未做。小内存现场（树莓派 Zero 级）请留意：
  一次拍照的瞬时占用可达数十 MiB，而 OOM 会跳过 `deactivate()`。
- 无记忆持久化（重启即忘）。
- 只实现了两个 OpenAI 兼容协议驱动；接非 OpenAI 形态的厂商（如 Anthropic）需要新写一个驱动
  （协议整形全在 Lua 侧，宿主不用改）。
- **摄像头只支持 MJPEG**：仅提供 YUYV 的设备会返回清晰 ERR（YUYV 要自带 JPEG 编码器或引入
  libjpeg，都比直接取 MJPEG 贵得多，而 USB 摄像头普遍支持 MJPEG）。树莓派 CSI 摄像头走
  libcamera、不经 V4L2 这条路径，也不在本驱动范围内。
- 驱动参数表的嵌套深度受 `kMaxLuaNesting`（8 层）约束：工具 schema 目前最深处为 6 层，留有余量但不宜再加嵌套。
