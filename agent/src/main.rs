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
mod budget;
mod config;
mod frame;
mod history;
mod hook;
mod ica;
mod linein;
mod model;
mod out;
mod placeholder;
mod prompt;
mod provider;
mod repl;
mod session;
mod signals;
mod summary;
mod toolspec;

use std::sync::Arc;
use std::time::Duration;

use crate::audit::AuditLog;
use crate::bridge::{Bridge, ConnParams};
use crate::budget::TaskBudget;
use crate::config::Config;
use crate::frame::{BackendClient, HARD_READ_CEILING};
use crate::hook::HookState;
use crate::out::{errln, out, outln};
use crate::prompt::PromptMaterial;
use crate::repl::Session;
use crate::session::{SessionFileWriter, SessionHead};
use crate::signals::install_signal_handlers;
use crate::toolspec::ToolSpec;

/// 连接后端的预算：后端上电 + 装结构脚本可能耗时（与 MCP sidecar 时代同值）。
const CONNECT_TIMEOUT_SEC: u64 = 120;

/// **重连**预算（区别于启动期）：后端中途重启一般在秒级完成；重连快失败、
/// 失败留给下一次调用再试，不长时间阻塞当次调用。
const RECONNECT_TIMEOUT_SEC: u64 = 30;

#[tokio::main]
async fn main() {
    harden_against_inspection();
    std::process::exit(run().await);
}

/// 进程自护（2026-09-25）：`prctl(PR_SET_DUMPABLE, 0)`。
///
/// 本进程内存里有模型 API key（`config.rs` 自环境变量或配置文件读入，`provider.rs`
/// 放进请求头）。docs/agent.md 安全一节此前如实写着「同 UID 不构成隔离：进程若能
/// ptrace Rust agent，就能从内存里把密钥捞出来」——non-dumpable 让内核把这条路收窄：
/// 同 UID 非特权进程的 `PTRACE_ATTACH` 与读 `/proc/<pid>/mem`、`environ` 被拒，崩溃
/// 不落 core。边界如实：root / 持 `CAP_SYS_PTRACE` 者不受影响，`/proc/<pid>/cmdline`
/// 依旧全局可读（所以两端都没有命令行传密钥的入口）。
///
/// 放在 tokio 运行时**之前**、`run()` 之前：密钥在 `config::build` 里读入，之后再设
/// 已经晚了。`BOT4CJ_ALLOW_PTRACE=1` 是给 gdb attach / strace -p 的显式逃生口，
/// stderr 明写「已放开」；prctl 失败只告警不退出（操作者必须知道防线缺位，但不该
/// 让 agent 因此起不起来）。与 C++ 宿主 `src/main.cpp` 的 `hardenAgainstInspection`
/// 同口径。
fn harden_against_inspection() {
    if std::env::var("BOT4CJ_ALLOW_PTRACE").is_ok_and(|v| v == "1") {
        errln!(
            "[mybot-agent] 注意: BOT4CJ_ALLOW_PTRACE=1，本进程保持可被同 UID ptrace / core dump\
             （仅调试用；模型密钥可自内存读出）"
        );
        return;
    }
    // SAFETY: prctl(PR_SET_DUMPABLE, 0) 只改本进程的一个内核标志，不碰任何内存；
    // 多余的三个参数按手册要求传 0。
    let rc = unsafe { libc::prctl(libc::PR_SET_DUMPABLE, 0, 0, 0, 0) };
    if rc != 0 {
        errln!(
            "[mybot-agent] 警告: prctl(PR_SET_DUMPABLE, 0) 失败（{}）；同 UID 进程可 ptrace 本进程 / 崩溃会落 core",
            std::io::Error::last_os_error()
        );
    }
}

/// `--resume` 恢复出来的会话三件套（历史消息 / 历史摘要 / 原 session id）。
#[derive(Debug)]
struct Resumed {
    history: Vec<rig_agent::completion::Message>,
    summary: Option<String>,
    session_id: String,
}

/// 会话持久化（批次 D）：`--resume` 的三道判据，放在系统提示词定稿之后、
/// 任何审计写之前（早一步还没有 preamble 可比，晚一步就核不出「提示词
/// 变没变」）——这条时序契约由调用点（run）持有，本函数只做判据本身：
///   ① 逐行解析（load_session_file 内部完成，坏行跳过并点名）；
///   ② 指纹核对（结构脚本名 / 工具集指纹 / 系统提示词指纹三者一致）；
///   ③ 协议自洽（tool_call_id 无孤儿/无重复/无漏答——服务端会 400 的序列
///      宁可当场拒绝）。
/// 任一不过即 Err（人话原因，调用方退出 2）；ICA sidecar 恢复也在本函数
/// （会话文件核对通过后才轮到它）。2026-09-29 B2 拆自 run()，报错文案
/// 与退出语义逐字保持。
async fn try_resume(
    cfg: &Config,
    bridge: &Bridge,
    tool_names: &[String],
    preamble: &str,
    material: &PromptMaterial,
) -> Result<Resumed, String> {
    resume_from(&cfg.session_file, bridge, tool_names, preamble, material).await
}

/// 恢复判据的接缝本体（2026-10-01 抽出：只依赖 session_file 一个配置项，
/// 可离线单测——Bridge 对临时 listener 构造即得，无需真后端/真 LLM）。
async fn resume_from(
    session_file: &str,
    bridge: &Bridge,
    tool_names: &[String],
    preamble: &str,
    material: &PromptMaterial,
) -> Result<Resumed, String> {
    let mut loaded = session::LoadedSession::default();
    let mut warnings = Vec::new();
    match session::load_session_file(&session_file, &mut loaded, &mut warnings) {
        Ok(()) => {
            for w in &warnings {
                errln!("[mybot-agent] {w}");
            }
            let expected = SessionHead {
                version: session::SESSION_FILE_VERSION,
                session_id: String::new(),
                robot: material.robot_name.clone(),
                tools_fp: session::tools_fingerprint(tool_names),
                system_fp: crate::audit::fingerprint64(preamble),
            };
            if let Some(reason) = session::head_mismatch(&expected, &loaded.head) {
                return Err(format!("会话恢复失败: {reason}"));
            }
            if let Some(reason) = session::protocol_violation(&loaded.messages) {
                return Err(format!(
                    "会话文件 '{}' 的历史不满足工具调用协议自洽（{reason}），\
                     拒绝恢复（这份历史送去模型服务端会被 400）",
                    session_file
                ));
            }
            errln!(
                "[mybot-agent] 已恢复会话：{} 条历史消息{}（来自 {}）",
                loaded.messages.len(),
                loaded
                    .summary
                    .as_ref()
                    .map(|s| format!("，含历史摘要 {} 字节", s.len()))
                    .unwrap_or_default(),
                session_file
            );
        }
        Err(e) => return Err(e),
    }
    bridge
        .ica
        .restore(&session_file)
        .map_err(|e| format!("ICA 会话恢复失败: {e}"))?;
    Ok(Resumed {
        history: loaded.messages,
        summary: loaded.summary,
        session_id: loaded.head.session_id,
    })
}

async fn run() -> i32 {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let cli = match config::parse_args(&args) {
        Ok(c) => c,
        Err(e) => return config_error(&e),
    };
    if cli.help {
        out!("{}", config::usage());
        return 0;
    }
    if cli.version {
        outln!("mybot-agent {}", env!("CARGO_PKG_VERSION"));
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
        out!("{}", config::summarize(&cfg));
        return 0;
    }
    if !cfg.config_warning.is_empty() {
        errln!("[mybot-agent] {}", cfg.config_warning);
    }
    if !cfg.thinking.is_empty() {
        // 如实告知：不静默丢（C++ 的驱动会把它翻译成各自的思维链开关；rig 没有
        // 对等的 provider 中立开关，本进程不猜形状）。
        errln!(
            "[mybot-agent] 提示: 配置里的 thinking={} 在本进程不生效（rig 无对等开关）；\
             需要思维链请用网关/模型侧的缺省设置",
            cfg.thinking
        );
    }

    // ---- 循环层共享状态 + 取消信号 ----
    // 先装信号处理器（连接后端可能要等上电，Ctrl-C 在这一段也该有效）
    let state = Arc::new(HookState::new(
        cfg.max_task_seconds,
        cfg.max_calls_per_round,
        TaskBudget::new(
            cfg.max_task_tool_result_bytes as usize,
            cfg.max_task_image_bytes as usize,
            cfg.max_task_seconds,
        ),
    ));
    install_signal_handlers(Arc::clone(&state));

    // ---- 后端：连接 → 读超时装配 → 首帧认证（可选） → 工具表 → 提示词素材 ----
    let assembly = match assemble_backend(&cfg, &state).await {
        Ok(a) => a,
        Err((msg, code)) => {
            errln!("错误: {msg}");
            return code;
        }
    };
    let BackendAssembly {
        bridge,
        specs,
        tool_names,
        material,
        preamble,
    } = assembly;

    // ---- 会话恢复（--resume；判据与时序契约见 try_resume）----
    let (resume_history, resume_summary, resume_session_id) = if cfg.resume {
        match try_resume(&cfg, &bridge, &tool_names, &preamble, &material).await {
            Ok(r) => (r.history, r.summary, r.session_id),
            Err(e) => return config_error(&e),
        }
    } else {
        (Vec::new(), None, String::new())
    };
    if !cfg.session_file.is_empty() {
        let w = session::perms_warning(&cfg.session_file);
        if !w.is_empty() {
            errln!("[mybot-agent] {w}");
        }
    }

    // ---- 审计：在任何模型/工具调用**之前**打开（坏路径要在这里就失败：
    //      「看起来在记、其实没记」比没有审计更糟）----
    let mut audit = if cfg.audit_log.is_empty() {
        None
    } else {
        match AuditLog::open(&cfg.audit_log) {
            Ok(a) => Some(a),
            Err(e) => {
                errln!("错误: {e}");
                return 2;
            }
        }
    };
    // 采纳被恢复会话的原 session id（恢复前后的行能对上同一段对话），必须在
    // 首行 start 写出前生效——此刻 `audit` 还没写过任何东西。
    if let Some(a) = audit.as_mut() {
        a.adopt_session_id(&resume_session_id);
    }

    // ---- 会话文件写入器（批次 D）：开新会话截断并写头；恢复则续写不重写头 ----
    let session_writer: Option<SessionFileWriter> = if cfg.session_file.is_empty() {
        None
    } else {
        let mut w = SessionFileWriter::new();
        if let Err(e) = w.open(&cfg.session_file, cfg.resume) {
            errln!("错误: {e}");
            return 2;
        }
        if !cfg.resume {
            let sid = audit
                .as_ref()
                .map(|a| a.session_id().to_string())
                .unwrap_or_default();
            w.write_header(&SessionHead {
                version: session::SESSION_FILE_VERSION,
                session_id: sid,
                robot: material.robot_name.clone(),
                tools_fp: session::tools_fingerprint(&tool_names),
                system_fp: crate::audit::fingerprint64(&preamble),
            });
        }
        Some(w)
    };

    let backend = match provider::build(&cfg, specs, &bridge, Arc::clone(&state), &preamble) {
        Ok(b) => b,
        Err(e) => {
            errln!("错误: {e:#}");
            return 2;
        }
    };
    // 就绪信息走 stderr：stdout 留给模型输出与交互界面。
    errln!(
        "[mybot-agent] 驱动 {} / 模型 {} / 接入点 {} / 工具 {} 个 / 后端 {}",
        backend.driver.as_str(),
        cfg.model,
        backend.effective_url,
        tool_names.len(),
        cfg.socket
    );

    let task = cli.task.clone();
    let resumed = cfg.resume;
    let mut session = Session::new(repl::SessionAssembly {
        cfg,
        backend,
        bridge,
        state,
        tool_names,
        material,
        audit,
        writer: session_writer,
        resume: repl::ResumeContext {
            history: resume_history,
            summary: resume_summary,
            resumed,
        },
    });
    if task.is_empty() {
        let rc = repl::run_repl(&mut session).await;
        session.write_session_end("end", "");
        rc
    } else {
        match repl::run_task_once(&mut session, &task).await {
            Ok(()) => {
                session.write_session_end("end", "");
                0
            }
            Err(e) => {
                errln!("错误: {e}");
                // 失败也要留一条会话结束记录——「这次会话怎么结束的」是复盘的第一句话
                session.write_session_end("error", &e);
                1
            }
        }
    }
}

/// 参数/配置错误的统一出口：stderr 报告 + 退出码 2（与 C++ agent 模式同口径）。
fn config_error(msg: &str) -> i32 {
    errln!("错误: {msg}");
    2
}

/// 后端连接 + 提示词素材装配的产物（`assemble_backend` 的返回值）。
struct BackendAssembly {
    bridge: Arc<Bridge>,
    specs: Vec<ToolSpec>,
    tool_names: Vec<String>,
    material: PromptMaterial,
    preamble: String,
}

/// 后端连接与提示词素材装配（run() 的中段，2026-10-01 拆出）。原 run() 这一段
/// 有 9 个「errln! + return 1|2」出口，与开头的配置出口交织成 12 处——
/// 收敛为 `Err((消息, 退出码))` 后 run() 只剩一条出口。退出码口径逐字保持：
/// 连接与传输失败 = 1；提示词素材/系统提示词超限等配置类 = 2。
async fn assemble_backend(
    cfg: &Config,
    state: &Arc<HookState>,
) -> Result<BackendAssembly, (String, i32)> {
    let mut client =
        BackendClient::connect_timeout(&cfg.socket, Duration::from_secs(CONNECT_TIMEOUT_SEC))
            .await
            .map_err(|e| (format!("无法连接后端 socket: {e}"), 1))?;
    // 读侧 deadline = 轮墙钟预算（--llm-task-timeout / max_task_seconds）：预算由此
    // 贯穿到工具调用内部（此前只判模型调用边界，后端活着但不回帧时无限挂起，
    // 2026-09-30 审计 P1）。0 = 不限 → 用 frame 的兜底上限。
    let read_timeout = if cfg.max_task_seconds > 0 {
        Duration::from_secs(cfg.max_task_seconds as u64)
    } else {
        HARD_READ_CEILING
    };
    client.set_read_timeout(read_timeout);
    // MCP-1（审计 P2-1）：后端设了 BOT4CJ_MCP_TOKEN 时，第一条帧必须是共享密钥
    // 认证。与 C++ 宿主读取同一个环境变量名（宿主在抹零前捕获，本进程在
    // prctl 收窄 /proc 下读取之后、此处第一次也是唯一一次读）。
    let mcp_token = std::env::var("BOT4CJ_MCP_TOKEN")
        .ok()
        .filter(|t| !t.is_empty());
    if let Some(token) = &mcp_token {
        client
            .authenticate(token)
            .await
            .map_err(|e| (e.to_string(), 1))?;
    }
    // 重连参数（连接判死后的下一次调用透明重连，见 bridge::Conn）：
    let conn_params = ConnParams {
        path: cfg.socket.clone(),
        token: mcp_token,
        connect_timeout: Duration::from_secs(RECONNECT_TIMEOUT_SEC),
        read_timeout,
    };
    let bridge = Arc::new(Bridge::new(client, conn_params, Arc::clone(state)));
    let specs = bridge
        .list_tools()
        .await
        .map_err(|e| (format!("取后端工具表失败: {e}"), 1))?;
    let tool_names: Vec<String> = specs.iter().map(|s| s.name.clone()).collect();
    let data = bridge
        .context_get()
        .await
        .map_err(|e| (format!("取后端上下文失败: {e}"), 1))?;
    let material = PromptMaterial::from_context(&data)
        // 退出码 2：与下方 build_system_prompt 的超限同属「提示词素材不
        // 合格」一类（口径：配置/素材错误 = 2；连接与传输失败 = 1）
        .map_err(|e| (e.to_string(), 2))?;
    let mut preamble =
        prompt::build_system_prompt(&cfg.driver, &cfg.llm_var, &cfg.prompt_pack, &material)
            .map_err(|e| (e.to_string(), 2))?;
    if bridge.ica.enabled() {
        preamble.push_str(ica::preamble_section());
        if preamble.len() > 65_536 {
            return Err(("开启 ICA 后系统提示词超过 65536 字节上限".to_string(), 2));
        }
    }
    Ok(BackendAssembly {
        bridge,
        specs,
        tool_names,
        material,
        preamble,
    })
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::budget::TaskBudget;
    use crate::frame::BackendClient;
    use crate::hook::HookState;
    use crate::prompt::PromptMaterial;
    use crate::session::{self, SessionHead};
    use std::os::unix::net::UnixListener as StdListener;
    use std::time::Duration;

    /// 与 bridge.rs 测试同款的离线构造：临时 listener + 立即接受的 BackendClient。
    /// try_resume 路径不发起任何帧调用（ICA 未启用时零行为），连接只需存在。
    async fn offline_bridge(tag: &str) -> Bridge {
        let dir = std::env::temp_dir().join(format!("mybot_resume_{}_{}", tag, std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("b.sock");
        let listener = StdListener::bind(&path).unwrap();
        let client =
            BackendClient::connect_timeout(&path.to_string_lossy(), Duration::from_secs(5))
                .await
                .unwrap();
        let _accepted = listener.accept().unwrap(); // 保持对端存活
        std::mem::forget(listener); // 对端保持存活即可,监听器不必关闭
        Bridge::new(
            client,
            crate::bridge::ConnParams {
                path: String::new(),
                token: None,
                connect_timeout: Duration::from_secs(1),
                read_timeout: Duration::from_secs(1),
            },
            std::sync::Arc::new(HookState::new(60, 0, TaskBudget::new(0, 0, 0))),
        )
    }

    fn material() -> PromptMaterial {
        PromptMaterial {
            robot_name: "relay-test".into(),
            ..Default::default()
        }
    }

    fn write_session(path: &std::path::Path, head: &SessionHead) {
        use std::io::Write;
        let mut w = session::SessionFileWriter::new();
        w.open(path.to_string_lossy().as_ref(), false).unwrap();
        w.write_header(head);
        let _ = std::fs::OpenOptions::new()
            .append(true)
            .open(path)
            .unwrap()
            .write(b"");
    }

    #[tokio::test]
    async fn resume_rejects_missing_and_mismatched_sessions() {
        let bridge = offline_bridge("missing").await;
        let dir = std::env::temp_dir().join(format!("mybot_resume_files_{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();

        // 文件不存在：错误如实冒出（fail-closed，不静默当新会话）
        let missing = dir.join("none.jsonl");
        let err = resume_from(
            missing.to_string_lossy().as_ref(),
            &bridge,
            &[],
            "p",
            &material(),
        )
        .await
        .unwrap_err();
        assert!(
            err.contains(&missing.to_string_lossy().to_string()),
            "{err}"
        );

        // 头不符（robot 名对不上）：点名「会话恢复失败」
        let session = dir.join("s.jsonl");
        write_session(
            &session,
            &SessionHead {
                version: session::SESSION_FILE_VERSION,
                session_id: String::new(),
                robot: "another-robot".into(),
                tools_fp: String::new(),
                system_fp: String::new(),
            },
        );
        let err = resume_from(
            session.to_string_lossy().as_ref(),
            &bridge,
            &[],
            "p",
            &material(),
        )
        .await
        .unwrap_err();
        assert!(err.contains("会话恢复失败"), "{err}");

        let _ = std::fs::remove_dir_all(&dir);
    }
}
