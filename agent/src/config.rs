//! config.rs —— LLM/agent 配置链（命令行 > 环境变量 > 配置文件 > 内置缺省）
//!
//! 语义**逐条镜像** C++ 侧（llmconfig.cpp + agentmode.cpp 的 buildAgentConfig），
//! 因为两侧常常吃同一份 `mybot.llm.json`：字段名、取值区间、未知字段拒绝、
//! 密钥来源链、H2 提示词根约束都按同一份判据走，否则「同一份配置两个进程两种
//! 理解」正是最难查的一类漂移。
//!
//! 与 C++ 侧的**如实差异**（都朝更严的一边偏，不静默）：
//!   * 模型名（model）在 Rust 侧必填：rig 需要显式的模型串，没有「驱动内置缺省」
//!     可回落。C++ 的 endpoint 无缺省 fail-closed 口径在这里扩到 model。
//!   * `allow_shell` / `allow_hot_sql` / `read_vars` / `camera_src_dir` 等**能力类**
//!     字段在本进程里只做「已知字段」登记（不解析）：能力由后端 `mybot --mcp-backend`
//!     自己的命令行决定，工具集经 `tool.list` 取回。本进程不持有、也无法授予任何
//!     能力，故这里忽略它们不会放宽任何东西（配置文件只能收紧不能放宽的约束
//!     由后端那一侧负责）。
//!   * 其余后端专属字段（摄像头、run_command 超时、字节预算……）同理只登记。

use std::fs;
use std::path::Path;

use serde_json::Value;

/// 配置文件读取上限（与 C++ `kMaxConfigBytes` 逐值对齐）。
pub const K_MAX_CONFIG_BYTES: u64 = 64 << 10;
/// 密钥文件读取上限（与 C++ `kMaxKeyFileBytes` 逐值对齐）。
pub const K_MAX_KEY_FILE_BYTES: u64 = 64 << 10;

/// 取值区间单点：与 C++ `llmconfig.hpp` 的 `llm_limits` 逐值对齐。
pub mod limits {
    pub const TIMEOUT_MIN_SEC: i64 = 1;
    pub const TIMEOUT_MAX_SEC: i64 = 600;
    pub const MAX_TOKENS_MIN: i64 = 1;
    pub const MAX_TOKENS_MAX: i64 = 200_000;
    pub const MAX_TURNS_MIN: i64 = 1;
    pub const MAX_TURNS_MAX: i64 = 100;
    pub const TASK_TIMEOUT_MIN_SEC: i64 = 0;
    pub const TASK_TIMEOUT_MAX_SEC: i64 = 86_400;
}

/// 内置缺省（与 C++ `AgentModeConfig` 的成员初值逐值对齐）。
pub const DEFAULT_DRIVER: &str = "LlmOpenAI";
pub const DEFAULT_LLM_VAR: &str = "llm0";
pub const DEFAULT_MAX_TURNS: i64 = 8;
pub const DEFAULT_MAX_TOKENS: i64 = 4096;
pub const DEFAULT_TEMPERATURE: f64 = 0.7;
pub const DEFAULT_TOP_P: f64 = 0.9;
pub const DEFAULT_TIMEOUT_SEC: i64 = 180;
pub const DEFAULT_MAX_TASK_SECONDS: i64 = 900;

/// 配置文件里被承认的全部字段名（C++ `loadConfigRoot` 的 kKnown 同集合）。
/// 未知字段一律拒绝——与「未知 --key 退出 2」同一口径，防拼错后静默失效。
const KNOWN_FIELDS: &[&str] = &[
    "driver",
    "var",
    "endpoint",
    "model",
    "api_key",
    "api_key_file",
    "timeout",
    "max_tokens",
    "temperature",
    "top_p",
    "max_task_seconds",
    "reasoning_effort",
    "thinking",
    "max_turns",
    "scripts_dir",
    "allow_shell",
    "allow_hot_sql",
    "prompt_files",
    "camera_var",
    "camera_device",
    "camera_src",
    "camera_src_dir",
    "allow_camera",
    "audit_log",
    "agent_policy",
    "read_vars",
    "session_file",
    "max_history_bytes",
    "max_task_tool_result_bytes",
    "max_task_image_bytes",
    "max_calls_per_round",
    "max_retries",
    "retry_backoff_ms",
    "run_command_timeout_sec",
    "run_command_max_timeout_sec",
    "action_script_timeout_sec",
];

/// 命令行原文（**只解析、不校验**；校验与合流在 [`build`]）。
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
    pub prompt_files: Vec<String>,
    pub audit_log: String,
    pub task: String,
    pub check_config: bool,
    pub help: bool,
    pub version: bool,
}

/// 已校验、已合流的生效配置。
#[derive(Debug, Clone)]
pub struct Config {
    pub socket: String,
    /// 实际使用的配置文件路径（空 = 未使用），用于如实告知。
    pub config_path: String,
    pub driver: String,
    pub llm_var: String,
    /// 完整接入点（如 `http://127.0.0.1:8080/v1/chat/completions`）。
    pub endpoint: String,
    pub model: String,
    /// **绝不进日志、绝不进 argv**：只用于注入请求头（rig 客户端装配）。
    pub api_key: String,
    /// 密钥来源枚举名（与审计 `api_key_source` 同口径：只报来源不报值）。
    pub api_key_source: &'static str,
    /// 配置文件相关告警（如密钥文件权限过松），空 = 无。
    pub config_warning: String,
    pub timeout_sec: i64,
    pub max_tokens: i64,
    pub temperature: f64,
    pub top_p: f64,
    pub max_turns: i64,
    pub max_task_seconds: i64,
    pub reasoning_effort: String,
    pub thinking: String,
    pub prompt_files: Vec<String>,
    /// 提示词包成品文本（空 = 未使用）。
    pub prompt_pack: String,
    pub audit_log: String,
}

/// 配置文件里本进程真正消费的字段（其余已知字段只登记不解析，见模块头注）。
#[derive(Debug, Default)]
struct FileConfig {
    driver: Option<String>,
    var: Option<String>,
    endpoint: Option<String>,
    model: Option<String>,
    api_key: Option<String>,
    api_key_file: Option<String>,
    timeout_sec: Option<i64>,
    max_tokens: Option<i64>,
    max_turns: Option<i64>,
    max_task_seconds: Option<i64>,
    temperature: Option<f64>,
    top_p: Option<f64>,
    reasoning_effort: Option<String>,
    thinking: Option<String>,
    prompt_files: Option<Vec<String>>,
    audit_log: Option<String>,
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
            "--prompt-file" => {
                let v = take("--prompt-file")?;
                cli.prompt_files.push(v);
            }
            "--audit-log" => cli.audit_log = take("--audit-log")?,
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
    s += "  --llm-task-timeout <秒>   单任务墙钟预算（0..86400，0 = 不限，缺省 900）\n\n";
    s += "其它:\n";
    s += "  --prompt-file <路径>   提示词包（文件或目录；可重复；命令行给出即整体覆盖配置文件列表）\n";
    s += "  --audit-log <路径|->   审计轨迹（JSON Lines；\"-\" = stderr；缺省关闭）\n";
    s += "  --task \"<任务>\"        一次性执行后退出（不给则进交互 REPL）\n";
    s += "  --check-config         只校验并打印配置摘要后退出（0 = 通过）\n";
    s += "  --help / --version\n";
    s
}

/// 是否「安全标识符」：字母/数字/下划线，且以字母或下划线开头
/// （与 C++ `util::isSafeIdent` 同判据）。
pub fn is_safe_ident(s: &str) -> bool {
    let mut it = s.chars();
    match it.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    it.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

fn env_var(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

/// 密钥环境变量链（与 C++ `resolveLlmKeyFromEnv` 逐序对齐）。
pub fn resolve_llm_key_from_env() -> Option<String> {
    for name in ["BOT4CJ_LLM_KEY", "ROBOT_AGENT_API_KEY", "DEEPSEEK_API_KEY"] {
        if let Some(v) = env_var(name) {
            return Some(v);
        }
    }
    None
}

/// 显式 "none" = 本次不读配置文件（大小写不敏感）。
fn is_config_disabled(s: &str) -> bool {
    s.trim().eq_ignore_ascii_case("none")
}

/// 缺省查找：`./mybot.llm.json` → `./mybot.llm.conf.json` → `$HOME/.config/mybot/llm.json`。
fn find_default_llm_config() -> Option<String> {
    for c in ["./mybot.llm.json", "./mybot.llm.conf.json"] {
        if Path::new(c).is_file() {
            return Some(c.to_string());
        }
    }
    if let Some(home) = env_var("HOME") {
        let p = format!("{home}/.config/mybot/llm.json");
        if Path::new(&p).is_file() {
            return Some(p);
        }
    }
    None
}

/// 配置文件选择：命令行 > `BOT4CJ_LLM_CONFIG` > 缺省查找。
fn pick_config_path(cli: &Cli) -> Option<String> {
    if !cli.llm_config.is_empty() {
        return (!is_config_disabled(&cli.llm_config)).then(|| cli.llm_config.clone());
    }
    if let Some(v) = env_var("BOT4CJ_LLM_CONFIG") {
        return (!is_config_disabled(&v)).then_some(v);
    }
    find_default_llm_config()
}

/// `~` 展开：只处理 `~` 与 `~/`（不做 `~user`，那要查 passwd）。
pub fn expand_home(path: &str) -> String {
    if !path.starts_with('~') || (path.len() > 1 && !path.starts_with("~/")) {
        return path.to_string();
    }
    match env_var("HOME") {
        Some(home) => format!("{home}{}", &path[1..]),
        None => path.to_string(),
    }
}

/// 读整个文件，超过 `cap` 即报错（**不静默截断**——截断过的内容进模型上下文
/// 比缺文件更难查，与 C++ `readWholeFileLimited` 同口径）。
pub fn read_whole_file_limited(path: &str, cap: u64, what: &str) -> Result<String, String> {
    let meta = fs::metadata(path).map_err(|e| format!("无法读取{what} '{path}': {e}"))?;
    if meta.len() > cap {
        return Err(format!(
            "{what} '{path}' 有 {} 字节，超过上限 {cap} 字节；请缩减内容后重试",
            meta.len()
        ));
    }
    fs::read(path)
        .map_err(|e| format!("无法读取{what} '{path}': {e}"))
        .map(|b| String::from_utf8_lossy(&b).into_owned())
}

// ---------------- 配置文件解析（严格：类型 / 区间 / 未知字段） ----------------

fn type_error(path: &str, key: &str, want: &str) -> String {
    format!("配置文件 {path} 的字段 '{key}' 需要{want}")
}

fn range_error(path: &str, key: &str, lo: f64, hi: f64, got: &str) -> String {
    format!("配置文件 {path} 的字段 '{key}' 需要在 [{lo}, {hi}] 范围内，实际是 {got}")
}

/// 字符串字段（空串按「未配置」处理，与 C++ `wantString` 同口径）。
fn want_string(root: &Value, path: &str, key: &str) -> Result<Option<String>, String> {
    match root.get(key) {
        None => Ok(None),
        Some(Value::String(s)) => Ok((!s.is_empty()).then(|| s.clone())),
        Some(_) => Err(type_error(path, key, "字符串")),
    }
}

/// 整数字段：区间 + 取整（非整数拒），与 C++ `wantInt` 同判据。
fn want_int(root: &Value, path: &str, key: &str, lo: i64, hi: i64) -> Result<Option<i64>, String> {
    let v = match root.get(key) {
        None => return Ok(None),
        Some(v) => v,
    };
    let n = match v {
        Value::Number(n) => n,
        _ => return Err(type_error(path, key, "数字")),
    };
    let f = n.as_f64().ok_or_else(|| type_error(path, key, "数字"))?;
    if !f.is_finite() || f < lo as f64 || f > hi as f64 {
        return Err(range_error(path, key, lo as f64, hi as f64, &f.to_string()));
    }
    if f.fract() != 0.0 {
        return Err(type_error(path, key, "整数"));
    }
    Ok(Some(f as i64))
}

/// 浮点字段（区间判在调用方，因为 temperature/top_p 的开闭区间不同）。
fn want_number(root: &Value, path: &str, key: &str) -> Result<Option<f64>, String> {
    let v = match root.get(key) {
        None => return Ok(None),
        Some(v) => v,
    };
    match v {
        Value::Number(n) => n
            .as_f64()
            .ok_or_else(|| type_error(path, key, "数字"))
            .map(Some),
        _ => Err(type_error(path, key, "数字")),
    }
}

/// 字符串数组字段（空数组按「未配置」；元素非字符串拒绝）。
fn want_string_array(root: &Value, path: &str, key: &str) -> Result<Option<Vec<String>>, String> {
    let v = match root.get(key) {
        None => return Ok(None),
        Some(v) => v,
    };
    let arr = match v {
        Value::Array(a) => a,
        _ => return Err(type_error(path, key, "字符串数组")),
    };
    let mut out = Vec::new();
    for item in arr {
        match item {
            Value::String(s) => {
                if !s.is_empty() {
                    out.push(s.clone());
                }
            }
            _ => return Err(format!("配置文件 {path} 的字段 '{key}' 的元素需要字符串")),
        }
    }
    Ok((!out.is_empty()).then_some(out))
}

/// 读取并解析配置文件（顶层对象 + 未知字段拒绝 + 类型/区间校验）。
fn load_config_file(path: &str) -> Result<FileConfig, String> {
    let text = read_whole_file_limited(path, K_MAX_CONFIG_BYTES, "配置文件")?;
    if text.trim().is_empty() {
        return Err(format!("配置文件为空: {path}"));
    }
    let root: Value =
        serde_json::from_str(&text).map_err(|e| format!("配置文件 {path} 不是合法 JSON: {e}"))?;
    let obj = match &root {
        Value::Object(o) => o,
        _ => return Err(format!("配置文件 {path} 的顶层必须是 JSON 对象")),
    };
    for k in obj.keys() {
        if !KNOWN_FIELDS.contains(&k.as_str()) {
            return Err(format!("配置文件 {path} 含未知字段 '{k}'（拼写错误？）"));
        }
    }

    let mut f = FileConfig {
        driver: want_string(&root, path, "driver")?,
        var: want_string(&root, path, "var")?,
        endpoint: want_string(&root, path, "endpoint")?,
        model: want_string(&root, path, "model")?,
        api_key: want_string(&root, path, "api_key")?,
        api_key_file: want_string(&root, path, "api_key_file")?,
        timeout_sec: want_int(
            &root,
            path,
            "timeout",
            limits::TIMEOUT_MIN_SEC,
            limits::TIMEOUT_MAX_SEC,
        )?,
        max_tokens: want_int(
            &root,
            path,
            "max_tokens",
            limits::MAX_TOKENS_MIN,
            limits::MAX_TOKENS_MAX,
        )?,
        max_turns: want_int(
            &root,
            path,
            "max_turns",
            limits::MAX_TURNS_MIN,
            limits::MAX_TURNS_MAX,
        )?,
        max_task_seconds: want_int(
            &root,
            path,
            "max_task_seconds",
            limits::TASK_TIMEOUT_MIN_SEC,
            limits::TASK_TIMEOUT_MAX_SEC,
        )?,
        temperature: want_number(&root, path, "temperature")?,
        top_p: want_number(&root, path, "top_p")?,
        reasoning_effort: want_string(&root, path, "reasoning_effort")?,
        prompt_files: want_string_array(&root, path, "prompt_files")?,
        audit_log: want_string(&root, path, "audit_log")?,
        thinking: None,
    };
    if let Some(t) = f.temperature {
        if !(0.0..=2.0).contains(&t) {
            return Err(format!(
                "配置文件 {path} 的字段 'temperature' 需要在 [0, 2] 范围内"
            ));
        }
    }
    if let Some(p) = f.top_p {
        if p <= 0.0 || p > 1.0 {
            return Err(format!(
                "配置文件 {path} 的字段 'top_p' 需要在 (0, 1] 范围内"
            ));
        }
    }
    // thinking 三态：字符串 enabled/disabled，或布尔（与 C++ parseConfigStrings 同判据）
    if let Some(v) = root.get("thinking") {
        f.thinking = match v {
            Value::String(s) => Some(s.clone()),
            Value::Bool(b) => Some(if *b { "enabled" } else { "disabled" }.to_string()),
            _ => {
                return Err(type_error(
                    path,
                    "thinking",
                    "字符串（enabled/disabled）或布尔值",
                ));
            }
        };
    }
    Ok(f)
}

/// 读取密钥文件（`~` 展开、首尾 trim、64 KiB 上限、空文件拒绝）。
pub fn read_api_key_file(path: &str) -> Result<String, String> {
    let real = expand_home(path);
    let text = read_whole_file_limited(&real, K_MAX_KEY_FILE_BYTES, "密钥文件")?;
    let key = text.trim();
    if key.is_empty() {
        return Err(format!("密钥文件为空: {real}"));
    }
    Ok(key.to_string())
}

/// 密钥字符集判据（C++ `validateApiKeyChars` 同判据）：含控制字符即拒绝
/// ——换行会被拼进 `Authorization` 头，等于把后面的内容变成额外请求头。
pub fn validate_api_key_chars(key: &str) -> Result<(), String> {
    for (i, b) in key.bytes().enumerate() {
        if b < 0x20 || b == 0x7f {
            return Err(format!(
                "含控制字符 0x{b:02x}（第 {} 个字节）：密钥必须是**单行**文本，\
                 换行会被原样拼进 Authorization 头，等于把后面的内容变成额外请求头；\
                 请检查密钥文件是否混入了多行内容或 CRLF 行尾",
                i + 1
            ));
        }
    }
    Ok(())
}

/// 组内/其他用户可读时给出提示（C++ `secretFilePermissionWarning` 同口径）。
pub fn secret_file_permission_warning(path: &str) -> String {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        if let Ok(st) = fs::metadata(expand_home(path)) {
            // S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH = 0o077
            if st.mode() & 0o077 == 0 {
                return String::new();
            }
            return format!(
                "提示: {} 对同组/其他用户可读（建议 chmod 600），它保存着 API key",
                expand_home(path)
            );
        }
        String::new()
    }
    #[cfg(not(unix))]
    {
        let _ = path;
        String::new()
    }
}

/// 由 CLI 组装生效配置（单点校验；失败给「人话报错」）。
pub fn build(cli: &Cli) -> Result<Config, String> {
    // ---- 0) socket：必填，不猜、不回落缺省路径 ----
    if cli.socket.trim().is_empty() {
        return Err(
            "缺少后端 socket 路径：用 --socket <路径> 指定（mybot --mcp-backend --mcp-socket 建立的那条）"
                .to_string(),
        );
    }

    // ---- 1) 配置文件（最低优先级的一层）----
    let config_path = pick_config_path(cli);
    let file = match &config_path {
        Some(p) => load_config_file(p)?,
        None => FileConfig::default(),
    };
    let config_path = config_path.unwrap_or_default();

    // ---- 2) 协议驱动与变量名 ----
    let driver = if !cli.llm_driver.is_empty() {
        cli.llm_driver.clone()
    } else {
        file.driver
            .clone()
            .unwrap_or_else(|| DEFAULT_DRIVER.to_string())
    };
    let llm_var = if !cli.llm_var.is_empty() {
        cli.llm_var.clone()
    } else {
        file.var
            .clone()
            .unwrap_or_else(|| DEFAULT_LLM_VAR.to_string())
    };
    if !is_safe_ident(&driver) {
        return Err(format!(
            "协议驱动表名必须是安全标识符（字母/数字/下划线），实际是 '{driver}'\
             （来自 --llm-driver 或配置文件的 driver 字段）"
        ));
    }
    if !is_safe_ident(&llm_var) {
        return Err(format!(
            "驱动变量名必须是安全标识符（字母/数字/下划线），实际是 '{llm_var}'\
             （来自 --llm-var 或配置文件的 var 字段）"
        ));
    }

    // ---- 3) endpoint / model：命令行 > 环境变量 > 配置文件；无内置缺省 ----
    let endpoint = if !cli.llm_endpoint.is_empty() {
        cli.llm_endpoint.clone()
    } else if let Some(v) = env_var("BOT4CJ_LLM_ENDPOINT") {
        v
    } else {
        file.endpoint.clone().unwrap_or_default()
    };
    if endpoint.is_empty() {
        return Err(
            "缺少模型接入点（endpoint）：用 --llm-endpoint 或配置文件的 endpoint 字段给出"
                .to_string()
                + "（没有内置缺省：Rust agent 不知道你要连哪个网关，猜一个只会把上下文发到错的地方）",
        );
    }
    let model = if !cli.llm_model.is_empty() {
        cli.llm_model.clone()
    } else if let Some(v) = env_var("BOT4CJ_LLM_MODEL") {
        v
    } else {
        file.model.clone().unwrap_or_default()
    };
    if model.is_empty() {
        return Err(
            "缺少模型名：用 --llm-model 或配置文件的 model 字段给出（rig 需要显式模型串，"
                .to_string()
                + "没有「驱动内置缺省」可回落）",
        );
    }

    // ---- 4) API key：环境变量链 > 配置文件 api_key > 配置文件 api_key_file ----
    let mut config_warning = String::new();
    let (api_key, api_key_source) = if let Some(k) = resolve_llm_key_from_env() {
        (k, "env")
    } else if let Some(k) = file.api_key.clone() {
        if !config_path.is_empty() {
            config_warning = secret_file_permission_warning(&config_path);
        }
        (k, "config_inline")
    } else if let Some(path) = file.api_key_file.clone() {
        let k = read_api_key_file(&path)
            .map_err(|e| format!("读取配置文件的 api_key_file 失败: {e}"))?;
        config_warning = secret_file_permission_warning(&path);
        (k, "config_file")
    } else {
        return Err(
            "缺少 API key：请设环境变量 BOT4CJ_LLM_KEY（也兼容 ROBOT_AGENT_API_KEY / DEEPSEEK_API_KEY），"
                .to_string()
                + "或在配置文件的 api_key / api_key_file 字段提供"
                + "（命令行不提供密钥选项：argv 对同用户进程可读）",
        );
    };
    if let Err(e) = validate_api_key_chars(&api_key) {
        let src = match api_key_source {
            "config_inline" => "配置文件 api_key 字段",
            "config_file" => "配置文件 api_key_file",
            _ => "环境变量",
        };
        return Err(format!("API key 非法（来源: {src}）: {e}"));
    }

    // ---- 5) 采样/超时/轮次：配置文件覆盖缺省，命令行再覆盖 ----
    let mut cfg = Config {
        socket: cli.socket.clone(),
        config_path,
        driver,
        llm_var,
        endpoint,
        model,
        api_key,
        api_key_source,
        config_warning,
        timeout_sec: file.timeout_sec.unwrap_or(DEFAULT_TIMEOUT_SEC),
        max_tokens: file.max_tokens.unwrap_or(DEFAULT_MAX_TOKENS),
        temperature: file.temperature.unwrap_or(DEFAULT_TEMPERATURE),
        top_p: file.top_p.unwrap_or(DEFAULT_TOP_P),
        max_turns: file.max_turns.unwrap_or(DEFAULT_MAX_TURNS),
        max_task_seconds: file.max_task_seconds.unwrap_or(DEFAULT_MAX_TASK_SECONDS),
        reasoning_effort: file.reasoning_effort.clone().unwrap_or_default(),
        thinking: file.thinking.clone().unwrap_or_default(),
        prompt_files: Vec::new(),
        prompt_pack: String::new(),
        audit_log: String::new(),
    };
    if !cli.llm_max_turns.is_empty() {
        cfg.max_turns = parse_int_option(&cli.llm_max_turns, "llm-max-turns", 1, 100)?;
    }
    if !cli.llm_max_tokens.is_empty() {
        cfg.max_tokens = parse_int_option(
            &cli.llm_max_tokens,
            "llm-max-tokens",
            limits::MAX_TOKENS_MIN,
            limits::MAX_TOKENS_MAX,
        )?;
    }
    if !cli.llm_timeout.is_empty() {
        cfg.timeout_sec = parse_int_option(
            &cli.llm_timeout,
            "llm-timeout",
            limits::TIMEOUT_MIN_SEC,
            limits::TIMEOUT_MAX_SEC,
        )?;
    }
    if !cli.llm_task_timeout.is_empty() {
        cfg.max_task_seconds = parse_int_option(
            &cli.llm_task_timeout,
            "llm-task-timeout",
            limits::TASK_TIMEOUT_MIN_SEC,
            limits::TASK_TIMEOUT_MAX_SEC,
        )?;
    }

    // ---- 6) 提示词包：命令行给过就整体覆盖配置文件列表（同 C++ 口径）----
    cfg.prompt_files = if !cli.prompt_files.is_empty() {
        cli.prompt_files.clone()
    } else {
        file.prompt_files.clone().unwrap_or_default()
    };

    // ---- 7) 审计落点：命令行 > 配置文件 ----
    cfg.audit_log = if !cli.audit_log.is_empty() {
        cli.audit_log.clone()
    } else {
        file.audit_log.clone().unwrap_or_default()
    };
    Ok(cfg)
}

/// 解析 CLI 里的整数字符串选项；越界/非数字给出带选项名的人话报错。
fn parse_int_option(raw: &str, key: &str, lo: i64, hi: i64) -> Result<i64, String> {
    let t = raw.trim();
    let v: i64 = t
        .parse()
        .map_err(|_| format!("选项 --{key} 需要一个整数，实际是 '{raw}'"))?;
    if v < lo || v > hi {
        return Err(format!(
            "选项 --{key} 需要在 [{lo}, {hi}] 范围内，实际是 {v}"
        ));
    }
    Ok(v)
}

/// 配置摘要（`--check-config`）：**只渲染、不判定**（校验单点在 [`build`]），
/// 密钥**只报来源不报值**（与 C++ `summarizeAgentConfig` 同契约）。
pub fn summarize(cfg: &Config) -> String {
    let or_none = |s: &str, fallback: &str| {
        if s.is_empty() {
            fallback.to_string()
        } else {
            s.to_string()
        }
    };
    let key_source = match cfg.api_key_source {
        "env" => "环境变量",
        "config_inline" => "配置文件 api_key 字段",
        "config_file" => "配置文件 api_key_file",
        _ => "未解析到",
    };
    let mut s = String::new();
    s += "配置校验通过：\n";
    s += &format!(
        "  配置文件    : {}\n",
        or_none(&cfg.config_path, "(未使用)")
    );
    s += &format!("  后端 socket : {}\n", cfg.socket);
    s += &format!("  协议驱动    : {}（变量 {}）\n", cfg.driver, cfg.llm_var);
    s += &format!("  模型        : {}\n", cfg.model);
    s += &format!("  接入点      : {}\n", cfg.endpoint);
    s += &format!(
        "  采样        : max_tokens={} temperature={} top_p={} timeout={}s\n",
        cfg.max_tokens, cfg.temperature, cfg.top_p, cfg.timeout_sec
    );
    s += &format!(
        "  轮次预算    : max_turns={}（rig 的模型调用预算，含重试）\n",
        cfg.max_turns
    );
    s += &format!("  API key     : {}（值不显示）\n", key_source);
    s += &format!(
        "  提示词包    : {} 项，{} 字节\n",
        cfg.prompt_files.len(),
        cfg.prompt_pack.len()
    );
    s += &format!(
        "  墙钟预算    : {}（单任务）\n",
        if cfg.max_task_seconds == 0 {
            "不限".to_string()
        } else {
            format!("{} 秒", cfg.max_task_seconds)
        }
    );
    s += &format!("  审计轨迹    : {}\n", or_none(&cfg.audit_log, "(未开启)"));
    if !cfg.config_warning.is_empty() {
        s += &format!("  配置告警    : {}\n", cfg.config_warning);
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_ident_matches_cpp_rule() {
        assert!(is_safe_ident("LlmOpenAI"));
        assert!(is_safe_ident("_x1"));
        assert!(!is_safe_ident("1abc")); // 数字开头
        assert!(!is_safe_ident("a-b")); // 连字符
        assert!(!is_safe_ident("")); // 空
        assert!(!is_safe_ident("表名")); // 非 ASCII
    }

    #[test]
    fn parse_args_rejects_unknown_option() {
        let err = parse_args(&["--bogus".to_string()]).unwrap_err();
        assert!(err.contains("未知选项"), "实际: {err}");
        let err = parse_args(&["--socket".to_string()]).unwrap_err();
        assert!(err.contains("需要一个值"), "实际: {err}");
        // 重复的 --prompt-file 累积（可重复选项）
        let cli = parse_args(&[
            "--prompt-file".into(),
            "a.md".into(),
            "--prompt-file".into(),
            "b.md".into(),
        ])
        .unwrap();
        assert_eq!(cli.prompt_files, vec!["a.md", "b.md"]);
    }

    #[test]
    fn build_is_fail_closed_on_missing_endpoint_and_model_and_key() {
        // 干净环境：不读任何配置文件（"none"），密钥链置空
        let base = Cli {
            socket: "/tmp/x.sock".into(),
            llm_config: "none".into(),
            ..Cli::default()
        };
        // 环境变量在本进程里可能被外部设置，故显式清掉这几条再断言
        // （edition 2021 下 set_var/remove_var 仍是安全函数；测试串行由 cargo 保证
        //  单测默认多线程，故本用例只读自己清过的这几条，不依赖其它用例）
        for k in [
            "BOT4CJ_LLM_KEY",
            "ROBOT_AGENT_API_KEY",
            "DEEPSEEK_API_KEY",
            "BOT4CJ_LLM_ENDPOINT",
            "BOT4CJ_LLM_MODEL",
        ] {
            std::env::remove_var(k);
        }
        let err = build(&base).unwrap_err();
        assert!(err.contains("缺少模型接入点"), "实际: {err}");

        let no_socket = Cli {
            llm_config: "none".into(),
            ..Cli::default()
        };
        assert!(build(&no_socket).unwrap_err().contains("socket 路径"));

        let with_ep = Cli {
            llm_endpoint: "http://127.0.0.1:1/v1/chat/completions".into(),
            ..base_cli()
        };
        let err = build(&with_ep).unwrap_err();
        assert!(err.contains("缺少模型名"), "实际: {err}");

        let with_model = Cli {
            llm_endpoint: "http://127.0.0.1:1/v1/chat/completions".into(),
            llm_model: "m".into(),
            ..base_cli()
        };
        let err = build(&with_model).unwrap_err();
        assert!(err.contains("缺少 API key"), "实际: {err}");

        std::env::set_var("BOT4CJ_LLM_KEY", "k");
        let cfg = build(&with_model).unwrap();
        assert_eq!(cfg.api_key_source, "env");
        assert_eq!(cfg.max_turns, DEFAULT_MAX_TURNS);
        assert_eq!(cfg.max_tokens, DEFAULT_MAX_TOKENS);
        std::env::remove_var("BOT4CJ_LLM_KEY");
    }

    fn base_cli() -> Cli {
        Cli {
            socket: "/tmp/x.sock".into(),
            llm_config: "none".into(),
            ..Cli::default()
        }
    }

    #[test]
    fn key_with_control_char_is_rejected() {
        assert!(validate_api_key_chars("sk-abc").is_ok());
        let err = validate_api_key_chars("sk-a\nbc").unwrap_err();
        assert!(err.contains("控制字符"), "实际: {err}");
        assert!(validate_api_key_chars("sk-\u{7f}").is_err());
        // 非 ASCII 合法（有些网关的密钥带 UTF-8）
        assert!(validate_api_key_chars("密钥").is_ok());
    }

    #[test]
    fn int_option_ranges_are_enforced() {
        assert_eq!(
            parse_int_option(" 12 ", "llm-max-turns", 1, 100).unwrap(),
            12
        );
        assert!(parse_int_option("0", "llm-max-turns", 1, 100).is_err());
        assert!(parse_int_option("101", "llm-max-turns", 1, 100).is_err());
        assert!(parse_int_option("x", "llm-max-turns", 1, 100).is_err());
        // 0 对 max_task_seconds 是合法值（不限）
        assert_eq!(
            parse_int_option("0", "llm-task-timeout", 0, 86_400).unwrap(),
            0
        );
    }
}
