//! mybot-agent —— bot4cj 的 Rust agent（rig 驱动；批次 B 骨架）
//!
//! 对上：直接面对操作者（交互 REPL 或一次性 `--task`）。
//! 对下：mybot 后端的帧通道（AF_UNIX，`mybot --mcp-backend` 建的 socket），
//! 工具全在那一侧执行——本进程不碰硬件、不含 Lua、不链接 C++。
//!
//! 装配顺序（每一步失败都在**做任何事之前**退出，不给「看起来在跑」的假象）：
//!   CLI → 配置（config）→ 提示词包（prompt）→ `--check-config` 提前返回
//!   → 连后端 → `tool.list`（动态工具）→ `context.get`（提示词素材）
//!   → 系统提示词 → 审计打开 + start 行 → rig 客户端装配 → 跑 REPL / --task。
//!
//! 退出码（与 C++ agent 模式同口径）：0 正常、1 运行期失败、2 配置/参数错误。
//! stdout 只有模型输出与交互界面；日志、告警、重试一律 stderr。

mod audit;
mod bridge;
mod config;
mod frame;
mod prompt;
mod provider;
mod repl;

use std::sync::Arc;
use std::time::Duration;

use crate::audit::AuditLog;
use crate::bridge::Bridge;
use crate::config::Config;
use crate::frame::BackendClient;
use crate::prompt::PromptMaterial;
use crate::repl::Session;

/// 连接后端的预算：后端上电 + 装结构脚本可能耗时（与 MCP sidecar 时代同值）。
const CONNECT_TIMEOUT_SEC: u64 = 120;

#[tokio::main]
async fn main() {
    std::process::exit(run().await);
}

async fn run() -> i32 {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let cli = match config::parse_args(&args) {
        Ok(c) => c,
        Err(e) => return config_error(&e),
    };
    if cli.help {
        print!("{}", config::usage());
        return 0;
    }
    if cli.version {
        println!("mybot-agent {}", env!("CARGO_PKG_VERSION"));
        return 0;
    }
    let mut cfg = match config::build(&cli) {
        Ok(c) => c,
        Err(e) => return config_error(&e),
    };
    if let Err(e) = load_prompt_pack(&mut cfg, &cli) {
        return config_error(&e);
    }
    if cli.check_config {
        print!("{}", config::summarize(&cfg));
        return 0;
    }
    if !cfg.config_warning.is_empty() {
        eprintln!("[mybot-agent] {}", cfg.config_warning);
    }
    if !cfg.thinking.is_empty() {
        // 如实告知：不静默丢（C++ 的驱动会把它翻译成各自的思维链开关；rig 没有
        // 对等的 provider 中立开关，本进程不猜形状）。
        eprintln!(
            "[mybot-agent] 提示: 配置里的 thinking={} 在本进程不生效（rig 无对等开关）；\
             需要思维链请用网关/模型侧的缺省设置",
            cfg.thinking
        );
    }

    // ---- 后端：连接 → 工具表 → 提示词素材 ----
    let client =
        match BackendClient::connect_timeout(&cfg.socket, Duration::from_secs(CONNECT_TIMEOUT_SEC))
            .await
        {
            Ok(c) => c,
            Err(e) => {
                eprintln!("错误: 无法连接后端 socket: {e}");
                return 1;
            }
        };
    let bridge = Arc::new(Bridge::new(client));
    let specs = match bridge.list_tools().await {
        Ok(s) => s,
        Err(e) => {
            eprintln!("错误: 取后端工具表失败: {e}");
            return 1;
        }
    };
    let tool_names: Vec<String> = specs.iter().map(|s| s.name.clone()).collect();
    let data = match bridge.context_get().await {
        Ok(d) => d,
        Err(e) => {
            eprintln!("错误: 取后端上下文失败: {e}");
            return 1;
        }
    };
    let material = match PromptMaterial::from_context(&data) {
        Ok(m) => m,
        Err(e) => {
            eprintln!("错误: {e}");
            return 1;
        }
    };
    let preamble =
        match prompt::build_system_prompt(&cfg.driver, &cfg.llm_var, &cfg.prompt_pack, &material) {
            Ok(p) => p,
            Err(e) => {
                eprintln!("错误: {e}");
                return 2;
            }
        };

    // ---- 审计：在任何模型/工具调用**之前**打开（坏路径要在这里就失败：
    //      「看起来在记、其实没记」比没有审计更糟）----
    let audit = if cfg.audit_log.is_empty() {
        None
    } else {
        match AuditLog::open(&cfg.audit_log) {
            Ok(a) => Some(a),
            Err(e) => {
                eprintln!("错误: {e}");
                return 2;
            }
        }
    };

    let backend = match provider::build(&cfg, specs, &bridge, &preamble) {
        Ok(b) => b,
        Err(e) => {
            eprintln!("错误: {e:#}");
            return 2;
        }
    };
    // 就绪信息走 stderr：stdout 留给模型输出与交互界面。
    eprintln!(
        "[mybot-agent] 驱动 {} / 模型 {} / 接入点 {} / 工具 {} 个 / 后端 {}",
        backend.driver.as_str(),
        cfg.model,
        backend.effective_url,
        tool_names.len(),
        cfg.socket
    );

    let task = cli.task.clone();
    let mut session = Session::new(cfg, backend, bridge, tool_names, material, audit);
    if task.is_empty() {
        let rc = repl::run_repl(&mut session).await;
        session.write_session_end("end", "");
        rc
    } else {
        match repl::run_task_once(&mut session, &task).await {
            Ok(text) => {
                println!("{text}");
                session.write_session_end("end", "");
                0
            }
            Err(e) => {
                eprintln!("错误: {e}");
                // 失败也要留一条会话结束记录——「这次会话怎么结束的」是复盘的第一句话
                session.write_session_end("error", &e);
                1
            }
        }
    }
}

/// 参数/配置错误的统一出口：stderr 报告 + 退出码 2（与 C++ agent 模式同口径）。
fn config_error(msg: &str) -> i32 {
    eprintln!("错误: {msg}");
    2
}

/// 提示词包：加载 + H2 根约束。
///
/// 根约束**只对配置文件来源的路径**生效（命令行 `--prompt-file` 是操作者本人授意，
/// 不受限）——理由见 prompt.rs：配置文件来源的路径若不受限，一份落在工作目录的
/// 配置文件就成了一条「读任意文件并外发」的链路。
fn load_prompt_pack(cfg: &mut Config, cli: &config::Cli) -> Result<(), String> {
    if cfg.prompt_files.is_empty() {
        return Ok(());
    }
    if cli.prompt_files.is_empty() {
        let mut roots = vec![".".to_string()];
        roots.extend(prompt::prompt_roots_from_env());
        prompt::prompt_paths_within_roots(&cfg.prompt_files, &roots)?;
    }
    cfg.prompt_pack = prompt::load_prompt_pack(&cfg.prompt_files)?;
    Ok(())
}
