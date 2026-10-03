//! config/cli.rs —— 命令行层：Cli 结构 + argv 解析 + 用法文本
//! （2026-09-29 B3 拆自 config.rs；语义与报错文案逐字保持）。

/// 命令行原文（**只解析、不校验**；校验与合流在 [`super::build`]）。
#[derive(Debug, Default)]
pub struct Cli {
    pub socket: String,
    pub llm_config: String,
    pub llm_driver: String,
    pub llm_var: String,
    pub llm_endpoint: String,
    pub llm_model: String,
    pub llm_timeout: String,
    pub llm_max_tokens: String,
    pub llm_max_turns: String,
    pub llm_task_timeout: String,
    /// `--llm-no-stream`：模型输出退回整段阻塞模式（缺省流式）。
    pub llm_no_stream: bool,
    pub prompt_files: Vec<String>,
    pub audit_log: String,
    /// 会话持久化落盘路径（批次 D）：只有**命令行给出**的路径才能用于 `--resume`
    /// 读取；配置文件的 `session_file` 只能用于写入（见 [`super::build`] 的判据）。
    pub session_file: String,
    pub resume: bool,
    pub task: String,
    pub check_config: bool,
    pub help: bool,
    pub version: bool,
}

/// 解析 argv（不含程序名）。未知选项 → Err（调用方按退出码 2 处理）。
pub fn parse_args(args: &[String]) -> Result<Cli, String> {
    let mut cli = Cli::default();
    let mut i = 0;
    while i < args.len() {
        let a = args[i].as_str();
        // 取下一个位置参数作为该选项的值；缺值立刻报错（不猜、不回落）。
        let mut take = |name: &str| -> Result<String, String> {
            i += 1;
            match args.get(i) {
                Some(v) => Ok(v.clone()),
                None => Err(format!("选项 {name} 需要一个值")),
            }
        };
        match a {
            "--socket" => cli.socket = take("--socket")?,
            "--llm-config" => cli.llm_config = take("--llm-config")?,
            "--llm-driver" => cli.llm_driver = take("--llm-driver")?,
            "--llm-var" => cli.llm_var = take("--llm-var")?,
            "--llm-endpoint" => cli.llm_endpoint = take("--llm-endpoint")?,
            "--llm-model" => cli.llm_model = take("--llm-model")?,
            "--llm-timeout" => cli.llm_timeout = take("--llm-timeout")?,
            "--llm-max-tokens" => cli.llm_max_tokens = take("--llm-max-tokens")?,
            "--llm-max-turns" => cli.llm_max_turns = take("--llm-max-turns")?,
            "--llm-task-timeout" => cli.llm_task_timeout = take("--llm-task-timeout")?,
            "--llm-no-stream" => cli.llm_no_stream = true,
            "--prompt-file" => {
                let v = take("--prompt-file")?;
                cli.prompt_files.push(v);
            }
            "--audit-log" => cli.audit_log = take("--audit-log")?,
            "--session-file" => cli.session_file = take("--session-file")?,
            "--resume" => cli.resume = true,
            "--task" => cli.task = take("--task")?,
            "--check-config" => cli.check_config = true,
            "--help" | "-h" => cli.help = true,
            "--version" | "-V" => cli.version = true,
            _ => {
                return Err(format!(
                    "未知选项 '{a}'（用 --help 看用法；配置文件拼错字段名同样会被拒绝）"
                ));
            }
        }
        i += 1;
    }
    Ok(cli)
}

/// 用法文本（`--help`）。
pub fn usage() -> String {
    let mut s = String::new();
    s += "mybot-agent —— bot4cj 的 Rust agent（rig 驱动；对下经 AF_UNIX 帧通道调用\n";
    s += "                `mybot --mcp-backend` 的工具层）\n\n";
    s += "用法: mybot-agent --socket <路径> [选项] [--task \"<任务>\"]\n\n";
    s += "必填:\n";
    s += "  --socket <路径>        后端 AF_UNIX socket（mybot --mcp-backend --mcp-socket 建的那条）\n";
    s += "  --llm-endpoint <URL>   模型接入点（完整 URL，如 http://127.0.0.1:8080/v1/chat/completions）\n";
    s += "  --llm-model <名>       模型名（rig 需要显式模型串，无内置缺省）\n";
    s += "  API key                环境变量 BOT4CJ_LLM_KEY（也兼容 ROBOT_AGENT_API_KEY /\n";
    s += "                         DEEPSEEK_API_KEY），或配置文件 api_key / api_key_file\n";
    s += "                         （**没有** --llm-key 选项：argv 对同用户进程可读）\n\n";
    s += "LLM 层（命令行 > 环境变量 > 配置文件 > 内置缺省）:\n";
    s += "  --llm-config <路径|none>  配置文件；none = 本次不读（缺省查找 ./mybot.llm.json、\n";
    s += "                            ./mybot.llm.conf.json、$HOME/.config/mybot/llm.json）\n";
    s += "  --llm-driver <表名>       协议驱动表名：LlmOpenAI | LlmDeepSeek | LlmAnthropic\n";
    s += "  --llm-var <名>            驱动变量名（仅用于提示词与提示符展示）\n";
    s += "  --llm-timeout <秒>        单次模型调用超时（1..600，缺省 180）\n";
    s += "  --llm-max-tokens <n>      单次响应上限（1..200000，缺省 4096）\n";
    s += "  --llm-max-turns <n>       模型调用预算（1..100，缺省 8）\n";
    s += "  --llm-task-timeout <秒>   单任务墙钟预算（0..86400，0 = 不限，缺省 900）\n";
    s += "  --llm-no-stream           模型输出退回整段阻塞模式（缺省流式：逐 delta 打到 stdout；\n";
    s += "                            也可用配置文件的 stream 字段，布尔）\n\n";
    s += "其它:\n";
    s += "  --prompt-file <路径>   提示词包（文件或目录；可重复；命令行给出即整体覆盖配置文件列表）\n";
    s += "  --audit-log <路径|->   审计轨迹（JSON Lines；\"-\" = stderr；缺省关闭）\n";
    s += "  --session-file <路径>  会话持久化落盘（JSON Lines；缺省关闭；也可用配置文件\n";
    s += "                         的 session_file 字段，但该来源只能写入，不能 --resume）\n";
    s += "  --resume               续写 --session-file 指向的既有会话（该路径必须来自命令行，\n";
    s += "                         且文件必须已存在；三道判据全过才恢复，见 docs/agent.md）\n";
    s += "  --task \"<任务>\"        一次性执行后退出（不给则进交互 REPL）\n";
    s += "  --check-config         只校验并打印配置摘要后退出（0 = 通过）\n";
    s += "  --help / --version\n";
    s
}
