//! repl.rs —— 会话与交互循环（`src/agentmode.cpp` 的 `AgentSession` + `runAgentRepl` 镜像）
//!
//! 三件事：
//!   1. **一轮任务**（`run_turn`）：任务开始前整块裁剪旧历史 → `begin_task()` 清
//!      任务内预算 → `runner(task).history(历史).max_turns(预算).run()`。成功把本轮
//!      新消息（含按位物化的图片后注）并进历史；失败**回滚到钩子记下的边界**（已完整
//!      跑完的轮留在历史里）——不留半截 tool_calls 序列，会话可以继续下一条。
//!   2. **审计**：model 行由本层写（turn/ok/status/error/ms/req+resp 字节/attempts/token，
//!      数据来自模型装饰器记的逐次调用记录）；session 行在 `Session::new`/
//!      `write_session_end`。**tool 行不在这里**——工具全经后端帧，后端
//!      `mybot --mcp-backend` 自己落（见 audit.rs 模块头）。
//!   3. **交互循环**（`run_repl`）：提示符、内置命令、单轮失败不结束会话；取消
//!      （SIGINT/SIGTERM）与 C++ 同口径——循环退出、会话照常收尾。
//!
//! 如实差异（记录在案）：
//!   * `PromptResponse.messages` 只含**本轮**新消息（rig 的 `new_messages`，不含传入
//!     的 chat_history）→ 这里拼成「旧历史 + 本轮」。
//!   * 图片注入只作用于**发出去的请求**（模型装饰器按请求重建的时机注入），rig 的
//!     transcript 里没有它 → 任务结束（或失败）后由 `hook::materialize` 按记录的
//!     位置插回会话历史，与 C++ 的「tool 消息之后另起一条 user 图片消息」同序。
//!   * `/hardware` / `/scripts` / `/sql` 在 C++ 是**本地**直读机器人与 `runSqlCommand`；
//!     本进程不持有硬件，一律经工具（`list_hardware` / `list_action_scripts` /
//!     `apply_sql` / `query_sql`）——所以「后端没给这个工具」时会明确报出缺哪个开关，
//!     不假装执行过。

use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::Instant;

use rig_agent::completion::{Message, PromptError};

use crate::audit::{AuditLog, ModelEntry, SessionEntry};
use crate::bridge::{Bridge, ToolOutcome};
use crate::config::Config;
use crate::hook::{self, CallRecord, HookState};
use crate::out::{errln, out, outln};
use crate::prompt::PromptMaterial;
use crate::provider::Backend;

/// `PromptError` → 操作者可见文本：剥掉 rig 的枚举前缀（`PromptCancelled: `）——
/// 钩子给的 reason 正是要让操作者看到的那句话（「执行被中断」「达到单任务墙钟预算
/// （N 秒）…」），C++ 侧的异常文本同样没有前缀。
fn format_prompt_error(e: PromptError) -> String {
    let s = e.to_string();
    match s.strip_prefix("PromptCancelled: ") {
        Some(rest) => rest.to_string(),
        None => s,
    }
}

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
    /// 循环层共享状态（预算/图片/回滚边界/取消标志）
    state: Arc<HookState>,
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
        state: Arc<HookState>,
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
            state,
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

    /// 跑一轮任务。`Err` = 本轮失败（历史已回滚到完整边界，会话可继续）。
    pub async fn run_turn(&mut self, task: &str) -> Result<String, String> {
        // 任务开始前：按字节预算**整块**裁掉最早的旧任务块（最新块无条件保留；
        // 0 = 不裁剪）。裁剪日志走 stderr：更早的任务上下文不再可见是必须告知的事实。
        crate::history::trim(
            &mut self.history,
            self.cfg.max_history_bytes as usize,
            &mut |t| errln!("{t}"),
        );
        // 任务内预算清零（含墙钟起点）与上一轮的观测值/图片清理
        self.state.begin_task();
        let snapshot = self.history.clone();

        // 借用只活在块内：`result` 是拥有所有权的值，块一结束 `self.backend` 的
        // 不可变借用就结束，后面才能改 `self.history`。
        let result = {
            let fut = self
                .backend
                .agent
                .runner(task.to_string())
                .history(snapshot.clone())
                .max_turns(self.cfg.max_turns as usize)
                .run();
            fut.await.map_err(format_prompt_error)
        };

        // 逐次模型调用的观测值（含重试与失败那一次）与图片注入批次
        let calls = self.state.drain_calls();
        let injections = self.state.take_injections();

        match result {
            Ok(resp) => {
                // 新消息（含 tool_calls / tool 结果）并进历史。空则退回「user + 最终
                // 文本」，宁可少记也不留空洞（历史里出现过半截序列是查不出来的坑）。
                let mut new = resp.messages.clone().unwrap_or_default();
                if new.is_empty() {
                    new.push(Message::user(task.to_string()));
                    new.push(Message::assistant(resp.output.clone()));
                }
                // 图片后注物化：注入只作用于发出去的请求，这里按记录的位置把
                // 「tool 消息之后的图片消息」插回会话历史（与 C++ 同序）。
                let mut full = snapshot;
                full.extend(new);
                self.history = hook::materialize(full, &injections);
                self.record_calls(&calls);
                Ok(resp.output)
            }
            Err(e) => {
                // 回滚：钩子记下的边界 = 已完整跑完的轮（含工具回执与其中的图片），
                // 半截的 tool_calls 序列不留在历史里。边界缺失（本轮连一次模型调用
                // 都没发生）时回滚到任务开始的历史。
                let base = self.state.rollback_boundary().unwrap_or(snapshot);
                self.history = hook::materialize(base, &injections);
                self.record_calls(&calls);
                Err(e)
            }
        }
    }

    /// 把逐次模型调用的观测值落成审计 model 行（turn 减一：rig 从 1 数起，
    /// C++ 的 turn 从 0 数起——两侧审计口径必须一致）+ 会话统计。
    fn record_calls(&mut self, calls: &[CallRecord]) {
        for rec in calls {
            self.stats.model_turns += 1;
            self.stats.model_ms += rec.ms.max(0) as u128;
            if rec.prompt_tokens.is_some() || rec.completion_tokens.is_some() {
                self.stats.last_prompt_tokens = rec.prompt_tokens;
                self.stats.last_completion_tokens = rec.completion_tokens;
            }
            self.write_model_row(ModelEntry {
                turn: rec.turn.saturating_sub(1) as i64,
                ok: rec.ok,
                status: if rec.ok { "ok" } else { "error" }.to_string(),
                error: rec.error.clone(),
                ms: rec.ms,
                req_bytes: rec.req_bytes,
                resp_bytes: rec.resp_bytes,
                attempts: rec.attempts,
                prompt_tokens: rec.prompt_tokens,
                completion_tokens: rec.completion_tokens,
            });
        }
    }

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
    outln!(
        "agent 模式：机器人已上电常驻（结构脚本 {}）。",
        session.material.structure_path
    );
    outln!(
        "直接输入任务；/hardware 看硬件清单，/scripts 看动作脚本，/sql 执行结构 SQL，\
         /status 看会话状态，/clear 清空对话记忆，/help 看帮助，/exit 退出。"
    );

    let mut lines = tokio::io::BufReader::new(tokio::io::stdin()).lines();
    loop {
        // D4：Ctrl-C / SIGTERM 之后不再接新任务（与 C++ `while (!stopRequested())`
        // 同口径：取消是操作者的意思，循环退出、会话照常收尾）
        if session.state.stop.load(Ordering::Relaxed) {
            break;
        }
        // 提示符 mybot[robotName|llmVar|Nhw]>（硬件数每轮重取：热插后不能少报）
        let hw = session.hardware_count().await;
        out!(
            "mybot[{}|{}|{}hw]> ",
            robot_name,
            session.cfg.llm_var,
            hw.map(|n| n.to_string()).unwrap_or_else(|| "?".to_string())
        );

        let line = match lines.next_line().await {
            Ok(Some(l)) => l,
            Ok(None) => break, // EOF（Ctrl-D）
            Err(e) => {
                errln!("错误: 读取输入失败: {e}");
                break;
            }
        };
        if line.len() > K_MAX_REPL_INPUT_BYTES {
            errln!(
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
            outln!("{HELP_TEXT}");
            continue;
        }
        if task == "/clear" {
            session.clear_history();
            outln!("已清空对话记忆（硬件保持上电，系统提示词不变）。");
            continue;
        }
        if task == "/status" {
            outln!("{}", session.status_text(hw));
            continue;
        }
        if task == "/hardware" {
            if !session.has_tool("list_hardware") {
                outln!("错误: 后端没有提供 list_hardware 工具（检查 mybot --mcp-backend 那一侧的开关）");
                continue;
            }
            outln!(
                "{}",
                session
                    .call_tool("list_hardware", serde_json::json!({}))
                    .await
            );
            continue;
        }
        if task == "/scripts" {
            if !session.has_tool("list_action_scripts") {
                outln!(
                    "错误: 后端没有提供 list_action_scripts 工具（检查 mybot --mcp-backend 那一侧的开关）"
                );
                continue;
            }
            outln!(
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
                outln!("{SQL_USAGE}");
                continue;
            }
            let (tool, flag) = if batch_has_write_ops(sql) {
                ("apply_sql", "--agent-hot-sql")
            } else {
                ("query_sql", "--agent-hot-sql")
            };
            if !session.has_tool(tool) {
                outln!(
                    "错误: 后端没有提供 {tool} 工具——结构 SQL 需要 mybot --mcp-backend 侧开 {flag}\
                     （配置文件不能放宽，只能收紧）"
                );
                continue;
            }
            outln!(
                "{}",
                session
                    .call_tool(tool, serde_json::json!({ "sql": sql }))
                    .await
            );
            continue;
        }

        match session.run_turn(task).await {
            Ok(text) => outln!("{text}"),
            Err(e) => {
                // REPL：单轮失败不结束会话（历史已回滚，可继续下一条任务）
                errln!("错误: {e}");
            }
        }
    }
    outln!("");
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
