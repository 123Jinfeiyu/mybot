//! config —— LLM/agent 配置链。**取值链按字段分列**（2026-10-01 批次 A 头注
//! 修正：此前一句「命令行 > 环境变量 > 配置文件 > 内置缺省」以偏概全——env 层
//! 只覆盖少数变量，且 api_key 的方向与其余字段相反）：
//!   * endpoint / model：命令行 > `BOT4CJ_LLM_ENDPOINT` / `BOT4CJ_LLM_MODEL`
//!     > 配置文件（无缺省，fail-closed）；
//!   * api_key：**env 链（`BOT4CJ_LLM_KEY` > `ROBOT_AGENT_API_KEY` >
//!     `DEEPSEEK_API_KEY`）> 文件内联 > 文件 `api_key_file`**——env 优先于
//!     文件，与其余字段方向相反（沿 C++ 既有口径；密钥不给命令行入口，防 argv 泄漏）；
//!   * 其余字段（驱动/变量名/采样/预算/流式/会话文件）：命令行 > 配置文件 >
//!     内置缺省，**无 env 层**；
//!   * 配置文件本身的选择：命令行 `--llm-config` > `BOT4CJ_LLM_CONFIG` >
//!     缺省查找 `./mybot.llm.json`。
//!
//! 语义**逐条镜像** C++ 侧（llmconfig.cpp + agentmode.cpp 的 buildAgentConfig），
//! 因为两侧常常吃同一份 `mybot.llm.json`：字段名、取值区间、未知字段拒绝、
//! 密钥来源链、H2 提示词根约束都按同一份判据走，否则「同一份配置两个进程两种
//! 理解」正是最难查的一类漂移。
//!
//! 2026-09-29 B3 分层：本文件只剩合流层（`Config` + `build` + 环境变量链 +
//! `summarize`）；命令行层在 [`cli`]，配置文件层（白名单/类型/区间/密钥文件，
//! 字段登记的单一事实源宏表）在 [`file`]。
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

mod cli;
mod file;
mod summarize;

pub use cli::Cli;
pub use cli::{parse_args, usage};
use file::{
    expand_home, load_config_file, read_api_key_file, secret_file_permission_warning,
    validate_api_key_chars, FileConfig,
};
pub use summarize::summarize;

use std::path::Path;

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
    /// 上下文字节预算的三条上限（与 C++ `kMaxHistoryBytesLimit` /
    /// `kMaxTaskToolResultBytesLimit` / `kMaxTaskImageBytesLimit` 逐值对齐）；
    /// 三者下界都是 0 = 不限。
    pub const BUDGET_BYTES_MAX: i64 = 64 << 20;
    /// 单轮工具调用数上限（0 不在此列：配置只允许 1..=1024，与 C++
    /// `kMaxCallsPerRoundMin/Max` 同区间；「不限」只能由代码缺省表达）
    pub const MAX_CALLS_PER_ROUND_MIN: i64 = 1;
    pub const MAX_CALLS_PER_ROUND_MAX: i64 = 1024;
    /// 网络重试尝试次数（含首次）：0 = 只发一次不重试（与 C++ 同口径）
    pub const MAX_RETRIES_MIN: i64 = 0;
    pub const MAX_RETRIES_MAX: i64 = 6;
    pub const RETRY_BACKOFF_MS_MIN: i64 = 0;
    pub const RETRY_BACKOFF_MS_MAX: i64 = 10_000;
    /// 历史摘要正文的字节上限区间：太小写不下任何有用信息，太大就失去了
    /// 「压缩」的意义（它是从 max_history_bytes 里扣出来的）。
    pub const SUMMARY_BYTES_MIN: i64 = 256;
    pub const SUMMARY_BYTES_MAX: i64 = 1 << 20;
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
/// 会话历史的字节预算（估重，见 history.rs）；0 = 不裁剪
pub const DEFAULT_MAX_HISTORY_BYTES: i64 = 4 << 20;
/// 被裁任务块的模型摘要正文上限（见 summary.rs）
pub const DEFAULT_MAX_SUMMARY_BYTES: i64 = 8192;
/// 任务内文本/图片预算（见 budget.rs）；0 = 不限
pub const DEFAULT_MAX_TASK_TOOL_RESULT_BYTES: i64 = 2 << 20;
pub const DEFAULT_MAX_TASK_IMAGE_BYTES: i64 = 4 << 20;
/// 单轮工具调用数上限（超出不派发、如实告知模型）
pub const DEFAULT_MAX_CALLS_PER_ROUND: i64 = 16;
/// 网络重试：尝试次数含首次（= 实际最多重试 n-1 次）与退避基础时长
pub const DEFAULT_MAX_RETRIES: i64 = 3;
pub const DEFAULT_RETRY_BACKOFF_MS: i64 = 1000;

/// 密钥包装（2026-09-30 审计 P1）：`Config` 曾派生 Debug 且含明文 `api_key`——
/// 当时没有任何 `{:?}` 打印它，但距离「一条 `errln!("{cfg:?}")` 把密钥写进
/// stderr/审计」只差一次手滑，与全仓「密钥只报来源不报值」的纪律不符。
/// 类型层面封死：`Debug`/`Display` 都只渲染 `<redacted>`，唯一取值口是
/// [`Secret::expose`]（调用点即用即弃，一眼可审——目前仅 provider.rs 装配
/// 请求头三处）。
#[derive(Clone)]
pub struct Secret(String);

impl Secret {
    pub fn new(s: String) -> Self {
        Self(s)
    }

    /// 唯一取值口：只应在装配请求头（provider.rs）处调用。
    pub fn expose(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Debug for Secret {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Secret(<redacted>)")
    }
}

impl std::fmt::Display for Secret {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("<redacted>")
    }
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
    /// [`Secret`] 包装：Debug/Display 均 redacted（见其文档）。
    pub api_key: Secret,
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
    /// 会话历史字节预算（0 = 不裁剪；整块裁掉最早的旧任务块）
    pub max_history_bytes: i64,
    /// 任务内文本/图片预算（0 = 不限；超限时把该次工具结果换成降级说明）
    pub max_task_tool_result_bytes: i64,
    pub max_task_image_bytes: i64,
    /// 单轮工具调用数上限（超出不派发）
    pub max_calls_per_round: i64,
    /// 网络重试尝试次数（含首次）与退避基础时长
    pub max_retries: i64,
    pub retry_backoff_ms: i64,
    pub reasoning_effort: String,
    pub thinking: String,
    /// 模型输出是否流式（逐 delta 打到 stdout）；false = 整段阻塞输出。
    pub stream: bool,
    /// 被裁任务块是否交给模型压成摘要挂在系统提示词末尾（见 summary.rs）
    pub history_summary: bool,
    /// 摘要正文字节上限（超出在字符边界截断并告警）
    pub max_summary_bytes: i64,
    pub prompt_files: Vec<String>,
    /// 提示词包成品文本（空 = 未使用）。
    pub prompt_pack: String,
    pub audit_log: String,
    /// 会话持久化落盘路径（空 = 不开启）。
    pub session_file: String,
    /// `session_file` 是否来自命令行（`--resume` 的判据：配置文件来源不能授权
    /// 恢复读取，只能收紧不能放宽，同 `camera_src_dir` / `read_vars` 口径）。
    pub session_file_from_cli: bool,
    pub resume: bool,
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

pub(crate) fn env_var(name: &str) -> Option<String> {
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

    // ---- 2) 协议驱动与变量名（拆解见 resolve_driver_and_var）----
    let (driver, llm_var) = resolve_driver_and_var(cli, &file)?;
    // ---- 3) endpoint / model（拆解见 resolve_endpoint_and_model）----
    let (endpoint, model) = resolve_endpoint_and_model(cli, &file)?;
    // ---- 4) API key（拆解见 resolve_api_key）----
    let (api_key, api_key_source, config_warning) = resolve_api_key(&file, &config_path)?;
    // ---- 5) 采样/超时/轮次：配置文件覆盖缺省，命令行再覆盖 ----
    let mut cfg = Config {
        socket: cli.socket.clone(),
        config_path,
        driver,
        llm_var,
        endpoint,
        model,
        api_key: Secret::new(api_key),
        api_key_source,
        config_warning,
        timeout_sec: file.timeout_sec.unwrap_or(DEFAULT_TIMEOUT_SEC),
        max_tokens: file.max_tokens.unwrap_or(DEFAULT_MAX_TOKENS),
        temperature: file.temperature.unwrap_or(DEFAULT_TEMPERATURE),
        top_p: file.top_p.unwrap_or(DEFAULT_TOP_P),
        max_turns: file.max_turns.unwrap_or(DEFAULT_MAX_TURNS),
        max_task_seconds: file.max_task_seconds.unwrap_or(DEFAULT_MAX_TASK_SECONDS),
        max_history_bytes: file.max_history_bytes.unwrap_or(DEFAULT_MAX_HISTORY_BYTES),
        max_task_tool_result_bytes: file
            .max_task_tool_result_bytes
            .unwrap_or(DEFAULT_MAX_TASK_TOOL_RESULT_BYTES),
        max_task_image_bytes: file
            .max_task_image_bytes
            .unwrap_or(DEFAULT_MAX_TASK_IMAGE_BYTES),
        max_calls_per_round: file
            .max_calls_per_round
            .unwrap_or(DEFAULT_MAX_CALLS_PER_ROUND),
        max_retries: file.max_retries.unwrap_or(DEFAULT_MAX_RETRIES),
        retry_backoff_ms: file.retry_backoff_ms.unwrap_or(DEFAULT_RETRY_BACKOFF_MS),
        reasoning_effort: file.reasoning_effort.clone().unwrap_or_default(),
        thinking: file.thinking.clone().unwrap_or_default(),
        // 命令行 --llm-no-stream 只能关不能开：配置文件 stream=false 时命令行没有
        // 「强制开」的选项——与其它字段「命令行 > 配置文件」的口径略有不同，因为
        // 关流式是保守方向，不需要放宽入口。
        stream: file.stream.unwrap_or(true) && !cli.llm_no_stream,
        history_summary: file.history_summary.unwrap_or(true),
        max_summary_bytes: file.max_summary_bytes.unwrap_or(DEFAULT_MAX_SUMMARY_BYTES),
        prompt_files: Vec::new(),
        prompt_pack: String::new(),
        audit_log: String::new(),
        session_file: String::new(),
        session_file_from_cli: false,
        resume: false,
    };
    if !cli.llm_max_turns.is_empty() {
        cfg.max_turns = parse_int_option(
            &cli.llm_max_turns,
            "llm-max-turns",
            limits::MAX_TURNS_MIN,
            limits::MAX_TURNS_MAX,
        )?;
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

    // ---- 8) 会话持久化（批次 D）：命令行 > 配置文件；读取只认命令行来源 ----
    //
    // 恢复是把磁盘内容读进模型上下文（输入通道），配置文件里的 session_file 只用于
    // 写入，**不能**授权一次读取——与 camera_src_dir / read_vars 同判据
    // （配置文件只能收紧不能放宽）。故 --resume 而路径来自配置文件时直接报错。
    cfg.session_file_from_cli = !cli.session_file.trim().is_empty();
    cfg.session_file = if cfg.session_file_from_cli {
        expand_home(cli.session_file.trim())
    } else {
        file.session_file
            .as_deref()
            .map(expand_home)
            .unwrap_or_default()
    };
    cfg.resume = cli.resume;
    if cfg.resume {
        if !cfg.session_file_from_cli {
            return Err(
                "选项 --resume 需要命令行显式给出 --session-file <路径>：恢复是把磁盘内容\
                 读进模型上下文（输入通道），配置文件里的 session_file 字段只用于写入，\
                 不能授权一次读取（配置文件只能收紧不能放宽）"
                    .to_string(),
            );
        }
        if !Path::new(&cfg.session_file).is_file() {
            return Err(format!(
                "会话文件不存在: {}（--resume 需要一个已存在的会话文件；路径来自命令行）",
                cfg.session_file
            ));
        }
    }
    Ok(cfg)
}

/// §2 协议驱动与变量名：命令行 > 配置文件 > 缺省；两名字都必须是安全标识符。
fn resolve_driver_and_var(cli: &Cli, file: &FileConfig) -> Result<(String, String), String> {
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

    Ok((driver, llm_var))
}

/// §3 endpoint/model：命令行 > 环境变量 > 配置文件；无内置缺省（猜一个只会把上下文发到错的地方）。
fn resolve_endpoint_and_model(cli: &Cli, file: &FileConfig) -> Result<(String, String), String> {
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

    Ok((endpoint, model))
}

/// §4 API key：环境变量链 > 配置文件 api_key > api_key_file；内联/文件来源要给权限告警。
fn resolve_api_key(
    file: &FileConfig,
    config_path: &str,
) -> Result<(String, &'static str, String), String> {
    let mut config_warning = String::new();
    let (api_key, api_key_source) = if let Some(k) = resolve_llm_key_from_env() {
        (k, "env")
    } else if let Some(k) = file.api_key.clone() {
        if !config_path.is_empty() {
            config_warning = secret_file_permission_warning(config_path);
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

    Ok((api_key, api_key_source, config_warning))
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

#[cfg(test)]
mod tests {
    use super::*;

    /// 环境变量是进程级全局，动它的用例必须彼此互斥——cargo 单测默认多线程
    /// 并行（502 行旧注释「测试串行由 cargo 保证」是错的），2026-10-01 基线
    /// 曾在 build_is_fail_closed… 与 config_debug_and_display… 之间竞出
    /// 「缺少 API key」假红（一方 remove_var 恰落在另一方 set_var 与 build 之间）。
    static ENV_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

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
        let _env = ENV_LOCK.lock().unwrap();
        // 干净环境：不读任何配置文件（"none"），密钥链置空
        // （单测默认多线程；本用例与其它动密钥链 env 的用例经 ENV_LOCK 互斥）
        let base = Cli {
            socket: "/tmp/x.sock".into(),
            llm_config: "none".into(),
            ..Cli::default()
        };
        // 环境变量在本进程里可能被外部设置，故显式清掉这几条再断言
        // （edition 2021 下 set_var/remove_var 仍是安全函数；单测默认多线程，
        //  与其它动密钥链 env 的用例经模块级 ENV_LOCK 互斥）
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

    /// 批次 C 起生效的六项：解析、0 = 显式不限、区间与未知字段判据。
    #[test]
    fn budget_and_retry_fields_are_parsed_and_range_checked() {
        let dir = std::env::temp_dir().join(format!("mybot_cfg_bc_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("cfg.json");
        let p = path.to_string_lossy().to_string();

        std::fs::write(
            &path,
            r#"{"max_history_bytes":0,"max_task_tool_result_bytes":1048576,
                "max_task_image_bytes":2048,"max_calls_per_round":1,
                "max_retries":0,"retry_backoff_ms":10000}"#,
        )
        .unwrap();
        let f = load_config_file(&p).unwrap();
        // 0 必须与「未配置」区分：显式 0 = 不限，None = 用缺省
        assert_eq!(f.max_history_bytes, Some(0));
        assert_eq!(f.max_task_tool_result_bytes, Some(1 << 20));
        assert_eq!(f.max_task_image_bytes, Some(2048));
        assert_eq!(f.max_calls_per_round, Some(1), "下界 1（0 不是合法上限）");
        assert_eq!(f.max_retries, Some(0), "0 = 只发一次不重试");
        assert_eq!(f.retry_backoff_ms, Some(10_000));

        // 越界 / 类型错 / 非法值一律拒绝（配置只能收紧，不能放宽）
        for (body, key) in [
            (r#"{"max_history_bytes":67108865}"#, "max_history_bytes"),
            (r#"{"max_history_bytes":-1}"#, "max_history_bytes"),
            (r#"{"max_calls_per_round":0}"#, "max_calls_per_round"),
            (r#"{"max_calls_per_round":1025}"#, "max_calls_per_round"),
            (r#"{"max_retries":7}"#, "max_retries"),
            (r#"{"retry_backoff_ms":10001}"#, "retry_backoff_ms"),
            (r#"{"max_task_image_bytes":"1M"}"#, "max_task_image_bytes"),
        ] {
            std::fs::write(&path, body).unwrap();
            let e = load_config_file(&p).unwrap_err();
            assert!(e.contains(key), "配置 {body} 应报 {key}：实际 {e}");
        }
        let _ = std::fs::remove_dir_all(&dir);
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

    /// ---- 会话持久化（批次 D）的组合校验：镜像 C++
    /// `sessionfile_resume_requires_cli_sourced_path` ----
    #[test]
    fn resume_requires_cli_sourced_session_file_path() {
        let dir = std::env::temp_dir().join(format!("mybot_cfg_resume_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let sess_path = dir.join("s.jsonl");
        std::fs::write(&sess_path, "x\n").unwrap();
        let sess_path_str = sess_path.to_string_lossy().to_string();
        // API key 经配置文件注入（**不**用环境变量：单测并行跑，env 会与其他用例
        // 的 set/remove 竞争——这正是不依赖全局态的好处）。
        let auth = dir.join("auth.json");
        std::fs::write(&auth, r#"{"api_key":"k"}"#).unwrap();
        let auth_str = auth.to_string_lossy().to_string();
        let base = || Cli {
            socket: "/tmp/x.sock".into(),
            llm_config: auth_str.clone(),
            llm_endpoint: "http://127.0.0.1:1/v1/chat/completions".into(),
            llm_model: "m".into(),
            ..Cli::default()
        };

        // --resume 不带 --session-file → 命令行来源判据未过
        {
            let cli = Cli {
                resume: true,
                ..base()
            };
            let err = build(&cli).unwrap_err();
            assert!(err.contains("命令行"), "实际: {err}");
        }
        // --resume + 不存在的路径
        {
            let cli = Cli {
                resume: true,
                session_file: dir.join("nope.jsonl").to_string_lossy().to_string(),
                ..base()
            };
            let err = build(&cli).unwrap_err();
            assert!(err.contains("不存在"), "实际: {err}");
        }
        // 合法组合：放行，且标记来源为命令行
        {
            let cli = Cli {
                resume: true,
                session_file: sess_path_str.clone(),
                ..base()
            };
            let cfg = build(&cli).unwrap();
            assert_eq!(cfg.session_file, sess_path_str);
            assert!(cfg.resume);
            assert!(cfg.session_file_from_cli);
        }
        // 不带 --resume 时，配置文件来源的 session_file 只用于写入（放行、不校验存在性）
        {
            let cfg_path = dir.join("cfg.json");
            std::fs::write(
                &cfg_path,
                format!(
                    r#"{{"api_key":"k","session_file":"{}"}}"#,
                    dir.join("unwritten.jsonl").to_string_lossy()
                ),
            )
            .unwrap();
            let cli = Cli {
                llm_config: cfg_path.to_string_lossy().to_string(),
                socket: "/tmp/x.sock".into(),
                llm_endpoint: "http://127.0.0.1:1/v1/chat/completions".into(),
                llm_model: "m".into(),
                ..Cli::default()
            };
            let cfg = build(&cli).unwrap();
            assert!(!cfg.session_file.is_empty());
            assert!(!cfg.session_file_from_cli);
            assert!(!cfg.resume);
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// ---- 配置文件 session_file 字段：已知字段（镜像 C++ `sessionfile_llmconfig_field`）----
    #[test]
    fn session_file_field_is_known_and_parsed() {
        let dir = std::env::temp_dir().join(format!("mybot_cfg_sf_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("ok.json");
        std::fs::write(&path, r#"{"session_file":"/tmp/x.jsonl"}"#).unwrap();
        let f = load_config_file(&path.to_string_lossy()).unwrap();
        assert_eq!(f.session_file.as_deref(), Some("/tmp/x.jsonl"));

        // 拼写错误的字段名仍被拒绝（未知字段判据不因新增字段而失效）
        let typo = dir.join("typo.json");
        std::fs::write(&typo, r#"{"session_fil":"x"}"#).unwrap();
        let e = load_config_file(&typo.to_string_lossy()).unwrap_err();
        assert!(e.contains("未知字段"), "实际: {e}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// `stream`：布尔字段；缺省流式；配置文件 false 或 --llm-no-stream 任一即关。
    #[test]
    fn stream_field_and_flag_compose_conservatively() {
        let dir = std::env::temp_dir().join(format!("mybot_cfg_stream_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let write = |name: &str, body: &str| {
            let p = dir.join(name);
            std::fs::write(&p, body).unwrap();
            p.to_string_lossy().to_string()
        };
        let on = write("on.json", r#"{"api_key":"k"}"#);
        let off = write("off.json", r#"{"api_key":"k","stream":false}"#);
        let bad = write("bad.json", r#"{"api_key":"k","stream":"yes"}"#);
        assert_eq!(load_config_file(&on).unwrap().stream, None);
        assert_eq!(load_config_file(&off).unwrap().stream, Some(false));
        assert!(load_config_file(&bad).unwrap_err().contains("stream"));

        let cli = |cfg: &str, no_stream: bool| Cli {
            socket: "/tmp/x.sock".into(),
            llm_config: cfg.to_string(),
            llm_endpoint: "http://127.0.0.1:1/v1/chat/completions".into(),
            llm_model: "m".into(),
            llm_no_stream: no_stream,
            ..Cli::default()
        };
        assert!(build(&cli(&on, false)).unwrap().stream, "缺省流式");
        assert!(
            !build(&cli(&on, true)).unwrap().stream,
            "--llm-no-stream 关"
        );
        assert!(
            !build(&cli(&off, false)).unwrap().stream,
            "配置 stream=false 关"
        );
        assert!(
            parse_args(&["--llm-no-stream".to_string()])
                .unwrap()
                .llm_no_stream
        );
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// 历史摘要（改进项 #7）：缺省开、8 KiB；只认布尔；字节上限有区间。
    #[test]
    fn history_summary_keys_default_on_and_are_range_checked() {
        let dir = std::env::temp_dir().join(format!("mybot_cfg_summary_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let write = |name: &str, body: &str| {
            let p = dir.join(name);
            std::fs::write(&p, body).unwrap();
            p.to_string_lossy().to_string()
        };
        let cli = |cfg: &str| Cli {
            socket: "/tmp/x.sock".into(),
            llm_config: cfg.to_string(),
            llm_endpoint: "http://127.0.0.1:1/v1/chat/completions".into(),
            llm_model: "m".into(),
            ..Cli::default()
        };
        let plain = write("plain.json", r#"{"api_key":"k"}"#);
        let cfg = build(&cli(&plain)).unwrap();
        assert!(cfg.history_summary, "缺省开");
        assert_eq!(cfg.max_summary_bytes, DEFAULT_MAX_SUMMARY_BYTES);

        let off = write(
            "off.json",
            r#"{"api_key":"k","history_summary":false,"max_summary_bytes":256}"#,
        );
        let cfg = build(&cli(&off)).unwrap();
        assert!(!cfg.history_summary);
        assert_eq!(cfg.max_summary_bytes, 256);
        assert!(summarize(&cfg).contains("history_summary=false"));

        for (body, key) in [
            (r#"{"history_summary":"yes"}"#, "history_summary"),
            (r#"{"history_summary":1}"#, "history_summary"),
            (r#"{"max_summary_bytes":255}"#, "max_summary_bytes"),
            (r#"{"max_summary_bytes":1048577}"#, "max_summary_bytes"),
            (r#"{"max_summary_bytes":"8k"}"#, "max_summary_bytes"),
        ] {
            let p = write("bad.json", body);
            let e = load_config_file(&p).unwrap_err();
            assert!(e.contains(key), "配置 {body} 应报 {key}：实际 {e}");
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn config_debug_and_display_never_leak_the_api_key() {
        let _env = ENV_LOCK.lock().unwrap();
        // 2026-09-30 审计 P1：Config 曾派生 Debug 且含明文密钥——距离一次
        // errln!("{cfg:?}") 把密钥写进 stderr/审计只差手滑。Secret 包装后，
        // 任何 {:?}/{} 渲染都只见 redacted；expose() 是唯一取值口。
        for k in ["BOT4CJ_LLM_KEY", "ROBOT_AGENT_API_KEY", "DEEPSEEK_API_KEY"] {
            std::env::remove_var(k);
        }
        std::env::set_var("BOT4CJ_LLM_KEY", "sk-DO-NOT-PRINT-9f8e7d");
        let cli = Cli {
            llm_endpoint: "http://127.0.0.1:9/v1/chat/completions".into(),
            llm_model: "m".into(),
            ..base_cli()
        };
        let cfg = build(&cli).unwrap();
        let dbg = format!("{cfg:?}");
        assert!(!dbg.contains("sk-DO-NOT-PRINT"), "Debug 泄密: {dbg}");
        assert!(dbg.contains("redacted"), "应可见 redacted 占位: {dbg}");
        let disp = format!("{}", cfg.api_key);
        assert!(!disp.contains("sk-"), "Display 泄密: {disp}");
        // 唯一取值口照常可用（provider.rs 装配请求头用）
        assert_eq!(cfg.api_key.expose(), "sk-DO-NOT-PRINT-9f8e7d");
        std::env::remove_var("BOT4CJ_LLM_KEY");
    }
}
/* 配置摘要渲染（summarize，--check-config 用）已下沉到 summarize.rs
 * （2026-10-01：纯 `s += &format!` 堆叠的渲染器独立成文件，mod.rs 只留
 * Config/limits/合流 build）。对外路径仍是 config::summarize（pub use 重导出）。 */
