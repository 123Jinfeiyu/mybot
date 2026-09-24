//! repl.rs —— 会话与交互循环（`src/agentmode.cpp` 的 `AgentSession` + `runAgentRepl` 镜像）
//!
//! 三件事：
//!   1. **一轮任务**（`run_turn`）：`runner(task).history(历史).max_turns(预算).run()`。
//!      成功把本轮新消息并进历史；失败**保留旧快照**（回滚）——不留半截 tool_calls
//!      序列，会话可以继续下一条（与 C++ `runTurn` 的 catch 同口径）。
//!   2. **审计**：model 行由本层写（turn/ok/status/error/ms/token）；session 行在
//!      `Session::new`/`write_session_end`。**tool 行不在这里**——工具全经后端帧，
//!      后端 `mybot --mcp-backend` 自己落（见 audit.rs 模块头）。
//!   3. **交互循环**（`run_repl`）：提示符、内置命令、单轮失败不结束会话。
//!
//! 如实差异（记录在案）：
//!   * `PromptResponse.messages` 只含**本轮**新消息（rig 的 `new_messages`，不含传入
//!     的 chat_history）→ 这里 `extend`，正好等于「旧历史 + 本轮」。
//!   * C++ 的 `/history` 裁剪、任务内字节预算、图片注入属循环层，批次 C/D 落。
//!   * `/hardware` / `/scripts` / `/sql` 在 C++ 是**本地**直读机器人与 `runSqlCommand`；
//!     本进程不持有硬件，一律经工具（`list_hardware` / `list_action_scripts` /
//!     `apply_sql` / `query_sql`）——所以「后端没给这个工具」时会明确报出缺哪个开关，
//!     不假装执行过。

use std::io::Write;
use std::sync::Arc;
use std::time::{Duration, Instant};

use rig_agent::completion::Message;

use crate::audit::{AuditLog, ModelEntry, SessionEntry};
use crate::bridge::{Bridge, ToolOutcome};
use crate::config::Config;
use crate::prompt::PromptMaterial;
use crate::provider::Backend;

/// REPL 单行输入上限（与 C++ `kMaxReplInputBytes` 逐值对齐）：超限**响亮拒绝**并
/// 提示，不静默截断（getline/read_line 本身无限制）。
pub const K_MAX_REPL_INPUT_BYTES: usize = 10 << 20;

/// 会话统计（`/status` 用）：只记真实发生过的数字，不推算。
#[derive(Debug, Default, Clone)]
pub struct SessionStats {
    pub model_turns: u64,
    pub model_ms: u128,
    pub last_prompt_tokens: Option<i64>,
    pub last_completion_tokens: Option<i64>,
}

pub struct Session {
    cfg: Config,
    backend: Backend,
    bridge: Arc<Bridge>,
    audit: Option<AuditLog>,
    /// 首轮 `context.get` 素材（结构脚本路径、动作脚本目录……；**硬件数每轮重取**，
    /// 因为后端的 /sql 热插会长出新硬件，缓存首轮值会让提示符一直少报）。
    material: PromptMaterial,
    /// 后端 `tool.list` 给的工具名（`/sql` 能力预检用：缺工具时点名后端开关，
    /// 而不是把一句含糊的「未知工具」丢给操作者）。
    tool_names: Vec<String>,
    /// rig transcript（旧历史 + 本轮新消息）；失败时保留旧快照
    history: Vec<Message>,
    stats: SessionStats,
    started: Instant,
}

impl Session {
    /// 建会话并**立刻**写审计的 session start 行（driver/model/接入点/密钥来源；
    /// 接入点在 audit 侧去凭据）。写不出 start 是审计自己的告警，不阻断会话。
    pub fn new(
        cfg: Config,
        backend: Backend,
        bridge: Arc<Bridge>,
        tool_names: Vec<String>,
        material: PromptMaterial,
        mut audit: Option<AuditLog>,
    ) -> Self {
        if let Some(a) = audit.as_mut() {
            a.write_session(&SessionEntry {
                status: "start".to_string(),
                driver: cfg.driver.clone(),
                model: cfg.model.clone(),
                endpoint: cfg.endpoint.clone(),
                api_key_source: cfg.api_key_source.to_string(),
                ..Default::default()
            });
        }
        Self {
            cfg,
            backend,
            bridge,
            audit,
            material,
            tool_names,
            history: Vec::new(),
            stats: SessionStats::default(),
            started: Instant::now(),
        }
    }

    /// `/clear`：只裁会话历史（硬件保持上电，系统提示词不变——rig 的 preamble 在
    /// agent 上，不在历史里）。
    pub fn clear_history(&mut self) {
        self.history.clear();
    }

    /// 会话结束行（`end` / `error`）。C++ 的 `writeSessionEnd` 同契约：失败会话也
    /// 必须留一条——「这次会话怎么结束的」是复盘的第一句话。
    pub fn write_session_end(&mut self, status: &str, error: &str) {
        let ms = self.started.elapsed().as_millis().min(i64::MAX as u128) as i64;
        if let Some(a) = self.audit.as_mut() {
            a.write_session(&SessionEntry {
                status: status.to_string(),
                error: error.to_string(),
                ms,
                ..Default::default()
            });
        }
    }

    /// 跑一轮任务。`Err` = 本轮失败（历史已回滚，会话可继续）。
    pub async fn run_turn(&mut self, task: &str) -> Result<String, String> {
        let turn = self.stats.model_turns as i64;
        let started = Instant::now();
        let budget = self.cfg.max_task_seconds;
        // 快照：失败回滚用（绝不把半截 tool_calls 序列留在历史里）
        let snapshot = self.history.clone();

        // 借用只活在块内：`result` 是拥有所有权的值，块一结束 `self.backend` 的
        // 不可变借用就结束，后面才能改 `self.history`。
        let result = {
            let fut = self
                .backend
                .agent
                .runner(task.to_string())
                .history(snapshot)
                .max_turns(self.cfg.max_turns as usize)
                .run();
            if budget > 0 {
                match tokio::time::timeout(Duration::from_secs(budget as u64), fut).await {
                    Ok(r) => r.map_err(|e| e.to_string()),
                    Err(_) => Err(format!(
                        "单任务墙钟预算 {budget} 秒已耗尽（--llm-task-timeout；0 = 不限）"
                    )),
                }
            } else {
                fut.await.map_err(|e| e.to_string())
            }
        };
        let ms = started.elapsed().as_millis().min(i64::MAX as u128) as i64;

        match result {
            Ok(resp) => {
                // 新消息（含 tool_calls / tool 结果）并进历史。空则退回「user + 最终
                // 文本」，宁可少记也不留空洞（历史里出现过半截序列是查不出来的坑）。
                let mut new = resp.messages.clone().unwrap_or_default();
                if new.is_empty() {
                    new.push(Message::user(task.to_string()));
                    new.push(Message::assistant(resp.output.clone()));
                }
                self.history.extend(new);
                self.stats.model_turns += 1;
                self.stats.model_ms += ms as u128;
                let pt = (resp.usage.input_tokens > 0).then_some(resp.usage.input_tokens as i64);
                let ct = (resp.usage.output_tokens > 0).then_some(resp.usage.output_tokens as i64);
                self.stats.last_prompt_tokens = pt;
                self.stats.last_completion_tokens = ct;
                self.write_model_row(ModelEntry {
                    turn,
                    ok: true,
                    status: "ok".to_string(),
                    ms,
                    prompt_tokens: pt,
                    completion_tokens: ct,
                    ..Default::default()
                });
                Ok(resp.output)
            }
            Err(e) => {
                // 回滚：`self.history` 原封不动（snapshot 是它的克隆）
                self.stats.model_turns += 1;
                self.stats.model_ms += ms as u128;
                self.write_model_row(ModelEntry {
                    turn,
                    ok: false,
                    status: "error".to_string(),
                    error: e.clone(),
                    ms,
                    ..Default::default()
                });
                Err(e)
            }
        }
    }

    /// 审计 model 行。**req_bytes/resp_bytes/attempts 本批次不写**：rig 把请求体
    /// 尺寸/尝试次数放在 completion hook 里（批次 C 用 `on_completion_response`
    /// 补），这里宁可缺字段也不填一个猜出来的数字。
    fn write_model_row(&mut self, entry: ModelEntry) {
        if let Some(a) = self.audit.as_mut() {
            a.write_model(&entry);
        }
    }

    /// 每轮重取硬件数（`context.get` 的 `hardwareCount`）。取不到 → `None`
    /// （提示符里显示 `?`，不编数字）。
    async fn hardware_count(&self) -> Option<u64> {
        let data = self.bridge.context_get().await.ok()?;
        data.get("hardwareCount")
            .and_then(serde_json::Value::as_u64)
    }

    /// `/status` 一览（只渲染、不判定；缺的数字给 `?`，不给假值）。
    fn status_text(&self, hw: Option<u64>) -> String {
        let st = &self.stats;
        let mut s = String::new();
        s += "会话状态:\n";
        s += &format!(
            "  模型/接入点 : {} @ {}（超时 {}s，最大 {} 轮模型调用）\n",
            self.cfg.model, self.backend.effective_url, self.cfg.timeout_sec, self.cfg.max_turns
        );
        s += &format!(
            "  本会话统计  : 模型 {} 轮/{}ms",
            st.model_turns, st.model_ms
        );
        if st.last_prompt_tokens.is_some() || st.last_completion_tokens.is_some() {
            let fmt = |v: Option<i64>| v.map(|x| x.to_string()).unwrap_or_else(|| "?".to_string());
            s += &format!(
                "，最近一次 token {}→{}",
                fmt(st.last_prompt_tokens),
                fmt(st.last_completion_tokens)
            );
        }
        s += "\n";
        s += &format!(
            "  对话历史    : {} 条消息（rig transcript）\n",
            self.history.len()
        );
        s += &format!(
            "  审计落点    : {}\n",
            match self.audit.as_ref() {
                Some(a) => format!("{}（session {}）", a.path(), a.session_id()),
                None => "(未开启)".to_string(),
            }
        );
        s += "  工具审计    : 工具行由后端 mybot --mcp-backend 落（含 exit_code/image_fp/summary），\
              本进程只记 session/model 行\n";
        s += &format!(
            "  硬件        : {}，{} 个变量（/hardware 看清单）\n",
            self.material.structure_path,
            hw.map(|n| n.to_string()).unwrap_or_else(|| "?".to_string())
        );
        s += &format!("  动作脚本目录: {}\n", self.material.scripts_dir);
        s
    }

    /// `tool_names` 里有没有这个工具（`/sql` 的能力预检）。
    fn has_tool(&self, name: &str) -> bool {
        self.tool_names.iter().any(|t| t == name)
    }

    /// 调一个工具并把回执渲染成给操作者看的文本（REPL 命令用；**不**进模型历史）。
    async fn call_tool(&self, name: &str, args: serde_json::Value) -> String {
        match self.bridge.call(name, args).await {
            Ok(o) => render_outcome(&o),
            Err(e) => format!("错误: {e}"),
        }
    }
}

/// 把工具回执渲染成一行文本：成功给回执文本，失败给「错误: …」（C++ 的 REPL 命令
/// 一律以「错误: 」前缀报告失败，这里同口径）。
fn render_outcome(o: &ToolOutcome) -> String {
    if o.ok {
        o.model_text()
    } else {
        format!("错误: {}", o.model_text())
    }
}

/// 一批 SQL 里有没有**变更类**语句（CREATE/INSERT/DELETE/DROP/UPDATE）——
/// 判据与 C++ `SqlStatementBatch.hasWriteOps` 同集合；「整批都是只读」才走
/// `query_sql`，混排批次里的写语句必须被看见（否则 `SELECT…; DELETE…` 会静默丢后半句）。
pub fn batch_has_write_ops(sql: &str) -> bool {
    for stmt in sql.split(';') {
        let first = stmt.split_whitespace().next().unwrap_or("");
        if ["CREATE", "INSERT", "DELETE", "DROP", "UPDATE"]
            .iter()
            .any(|k| first.eq_ignore_ascii_case(k))
        {
            return true;
        }
    }
    false
}

/// `/sql` 的用法串（空语句时打印；与 C++ `runAgentRepl` 同文本）。
const SQL_USAGE: &str = "用法: /sql <语句>（以 ; 结尾），例如 /sql SELECT * FROM Vars;\n\
     可加等值条件（AND 连接），例如 /sql SELECT * FROM Servo WHERE name = 's1';\n\
     SHOW TABLES 列出当前结构的全部表名与数据行数，例如 /sql SHOW TABLES;\n\
     DELETE 会当场断电摘除匹配行且不可逆，例如 /sql DELETE FROM Servo WHERE name = 's1';\n\
     UPDATE 按断电重建改写命中行（先断电摘除再重装），例如 /sql UPDATE Servo SET angle = 90 WHERE name = 's1';";

const HELP_TEXT: &str = "内置命令:\n\
  /hardware  - 显示硬件清单（只读，不触发任何动作）\n\
  /scripts   - 显示可用动作脚本清单（相对路径 + 首行注释）\n\
  /sql <语句> - 执行结构 SQL：SELECT * FROM t [WHERE 列 = 值 [AND ...]] / SHOW TABLES 只读查询运行时矩阵（SHOW TABLES 列出全部表名与数据行数）；\n\
               CREATE TABLE / INSERT INTO 立即带电生效（失败全回滚）；\n\
               DELETE FROM t [WHERE 列 = 值 [AND ...]] 当场断电摘除匹配行（不可逆）；\n\
               UPDATE t SET 列 = 值 [, ...] [WHERE ...] 按断电重建改写命中行（删旧件 + 装新件）\n\
  /status    - 会话状态一览（历史/预算/用量/审计落点，只读）\n\
  /clear     - 清空对话记忆（保留硬件上电状态与系统提示词）\n\
  /help      - 显示此帮助\n\
  /exit      - 退出 agent 模式\n\
  /quit      - 同 /exit\n\
直接输入任务描述即可让智能体执行，例如：\n\
  让机器人站立\n\
  拍照并描述看到的内容";

/// 交互 REPL（C++ `runAgentRepl` 镜像）。返回进程退出码（恒 0：退出是操作者的
/// 意思，不是失败）。日志/告警走 stderr，界面走 stdout。
pub async fn run_repl(session: &mut Session) -> i32 {
    use tokio::io::AsyncBufReadExt;

    let robot_name = if session.material.robot_name.is_empty() {
        "(未命名)".to_string()
    } else {
        session.material.robot_name.clone()
    };
    println!(
        "agent 模式：机器人已上电常驻（结构脚本 {}）。",
        session.material.structure_path
    );
    println!(
        "直接输入任务；/hardware 看硬件清单，/scripts 看动作脚本，/sql 执行结构 SQL，\
         /status 看会话状态，/clear 清空对话记忆，/help 看帮助，/exit 退出。"
    );

    let mut lines = tokio::io::BufReader::new(tokio::io::stdin()).lines();
    loop {
        // 提示符 mybot[robotName|llmVar|Nhw]>（硬件数每轮重取：热插后不能少报）
        let hw = session.hardware_count().await;
        print!(
            "mybot[{}|{}|{}hw]> ",
            robot_name,
            session.cfg.llm_var,
            hw.map(|n| n.to_string()).unwrap_or_else(|| "?".to_string())
        );
        let _ = std::io::stdout().flush();

        let line = match lines.next_line().await {
            Ok(Some(l)) => l,
            Ok(None) => break, // EOF（Ctrl-D）
            Err(e) => {
                eprintln!("错误: 读取输入失败: {e}");
                break;
            }
        };
        if line.len() > K_MAX_REPL_INPUT_BYTES {
            eprintln!(
                "错误: 输入过长（{} 字节，上限 {K_MAX_REPL_INPUT_BYTES} 字节）；\
                 请缩短输入或分多次提交",
                line.len()
            );
            continue;
        }
        let task = line.trim();
        if task.is_empty() {
            continue;
        }
        if task == "/exit" || task == "/quit" {
            break;
        }
        if task == "/help" {
            println!("{HELP_TEXT}");
            continue;
        }
        if task == "/clear" {
            session.clear_history();
            println!("已清空对话记忆（硬件保持上电，系统提示词不变）。");
            continue;
        }
        if task == "/status" {
            println!("{}", session.status_text(hw));
            continue;
        }
        if task == "/hardware" {
            if !session.has_tool("list_hardware") {
                println!("错误: 后端没有提供 list_hardware 工具（检查 mybot --mcp-backend 那一侧的开关）");
                continue;
            }
            println!(
                "{}",
                session
                    .call_tool("list_hardware", serde_json::json!({}))
                    .await
            );
            continue;
        }
        if task == "/scripts" {
            if !session.has_tool("list_action_scripts") {
                println!(
                    "错误: 后端没有提供 list_action_scripts 工具（检查 mybot --mcp-backend 那一侧的开关）"
                );
                continue;
            }
            println!(
                "{}",
                session
                    .call_tool("list_action_scripts", serde_json::json!({}))
                    .await
            );
            continue;
        }
        // /sql <语句>：全只读走 query_sql；含变更语句走 apply_sql（后端那侧才真正
        // 带电生效/断电摘除）。必须在本派发点 continue —— 否则会被当任务发给模型。
        if task.starts_with("/sql")
            && matches!(
                task.as_bytes()[4..].first(),
                None | Some(b' ') | Some(b'\t')
            )
        {
            let sql = task[4..].trim();
            if sql.is_empty() {
                println!("{SQL_USAGE}");
                continue;
            }
            let (tool, flag) = if batch_has_write_ops(sql) {
                ("apply_sql", "--agent-hot-sql")
            } else {
                ("query_sql", "--agent-hot-sql")
            };
            if !session.has_tool(tool) {
                println!(
                    "错误: 后端没有提供 {tool} 工具——结构 SQL 需要 mybot --mcp-backend 侧开 {flag}\
                     （配置文件不能放宽，只能收紧）"
                );
                continue;
            }
            println!(
                "{}",
                session
                    .call_tool(tool, serde_json::json!({ "sql": sql }))
                    .await
            );
            continue;
        }

        match session.run_turn(task).await {
            Ok(text) => println!("{text}"),
            Err(e) => {
                // REPL：单轮失败不结束会话（历史已回滚，可继续下一条任务）
                eprintln!("错误: {e}");
            }
        }
    }
    println!();
    0
}

/// 一次性任务（`--task`）：成功返回 `Ok(输出)`，失败 `Err(错误文本)`。
/// 退出码与结束行的分派在 main（`end`/0 或 `error`/1）。
pub async fn run_task_once(session: &mut Session, task: &str) -> Result<String, String> {
    session.run_turn(task).await
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sql_batch_write_op_detection_matches_cpp_set() {
        // 全只读 → query_sql 通道
        assert!(!batch_has_write_ops("SELECT * FROM Servo;"));
        assert!(!batch_has_write_ops(
            "  select * from Vars where name = 'a'; SHOW TABLES;"
        ));
        assert!(!batch_has_write_ops(""));
        // 大小写不敏感 + 混排批次里的写语句必须被看见
        assert!(batch_has_write_ops("create table X (a INT);"));
        assert!(batch_has_write_ops(
            "SELECT * FROM T; INSERT INTO T VALUES ('x');"
        ));
        for kw in [
            "DELETE FROM T;",
            "DROP TABLE T;",
            "UPDATE T SET a = 1;",
            "insert into t values (1);",
        ] {
            assert!(batch_has_write_ops(kw), "应判为变更语句: {kw}");
        }
        // 关键字只在行首算：列/值里出现 create 不算变更
        assert!(!batch_has_write_ops(
            "SELECT * FROM T WHERE note = 'create table';"
        ));
    }

    #[test]
    fn render_outcome_prefixes_errors_only() {
        let ok = ToolOutcome {
            text: "硬件清单：\n  relay0".to_string(),
            ok: true,
            ..Default::default()
        };
        assert_eq!(render_outcome(&ok), "硬件清单：\n  relay0");
        let bad = ToolOutcome {
            text: "设备忙".to_string(),
            ok: false,
            error: "设备忙".to_string(),
            ..Default::default()
        };
        assert_eq!(render_outcome(&bad), "错误: 设备忙");
    }
}
