//! bridge.rs —— 后端工具层桥接（`src/mcpbackend.cpp` 的镜像；MCP 版 `handler.rs` 的后继）
//!
//! 工具是**动态**的：运行期从后端 `tool.list` 拉（随后端启动开关
//! `--agent-shell` / `--agent-hot-sql` / `--read-var` / camera 变化），schema 原样
//! 透传给 rig，不做编译期注册——这正是换成 rig 动态工具的理由。
//!
//! 帧协议（与 `routeToolList` / `routeContextGet` / `routeToolCall` 逐字段对齐）：
//!   `tool.list`    → `data.tools[]{name,description,inputSchema}`
//!   `context.get`  → `data.{robotName,structurePath,scriptsDir,allowShell,allowHotSql,
//!                     hardwareCount,structureExcerpt,hardwareList,scriptsList,readableVars}`
//!   `tool.call`    → `data.{text,imageDataUrl,ok,error,executed,paramError,summary}`（七字段）
//!
//! 工具失败语义：后端 `data.ok=false` 时把**回执文本**（`text`，空则退 `error`）
//! 交回模型——rig 的 `ToolExecutionError::other` 把它同时作为模型可见输出，
//! 于是模型看到的正是后端那句话说清楚了什么错（与 C++ 派发漏斗同口径）。
//!
//! 图片（批次 C）：后端 `imageDataUrl` 在预算内时交给 `HookState` 的待注入队列——
//! C++ 侧把图片作为 tool 消息**之后**的独立 user 消息注入，rig 侧的等价落点是
//! 模型装饰器（model.rs：追加到含工具结果的那条 user 消息末尾，转换到线上与 C++
//! 同序）。回执（tool 消息）本身只给文本：OpenAI 协议不允许工具结果里带图片。
//!
//! 任务内预算（AG-4/AG-21）在这一层判：工具回执到手即 `admit`（执行过才降级，
//! 判定在**执行之后**——体积要拿到结果才知道，而副作用已经发生，降级说明必须
//! 如实写明「已执行」）。语义单点在 budget.rs。

use std::sync::Arc;
use std::sync::Mutex as StdMutex;
use std::time::Duration;

use rig_agent::tool::DynamicTool;
use rig_core::tool::{PortableDynamicTool, ToolExecutionError, ToolOutput};
use serde_json::{Map, Value};
use tokio::sync::Mutex;

use crate::frame::BackendClient;
use crate::hook::HookState;
use crate::ica::IcaLedger;
use crate::out::errln;

/// 一条工具描述（后端 `tool.list` 的一条）。
#[derive(Debug, Clone)]
pub struct ToolSpec {
    pub name: String,
    pub description: String,
    /// JSON Schema object（非对象时回落 `{"type":"object"}`，不让一个坏 schema
    /// 整条 list 失败）
    pub parameters: Value,
}

/// 一条工具回执的七个字段（后端 `data`）。
/// 与后端 `AgentToolResult`（src/agenttools.hpp，JSON 键序列化在
/// src/mcpbackend.cpp 的 routeToolCall）逐字段对应 camel↔snake：加/删/改字段
/// 必须双侧同步——C++ 侧 test_mcpbackend 的
/// mcpbackend_toolresult_schema_matches_rust_outcome 读本文件比对钉住。
#[derive(Debug, Clone, Default)]
pub struct ToolOutcome {
    pub text: String,
    pub image_data_url: String,
    pub ok: bool,
    pub error: String,
    pub executed: bool,
    pub param_error: bool,
    pub summary: String,
}

impl ToolOutcome {
    /// 模型可见输出：工具消息只允许文本（OpenAI 协议），故这里只给文本块；
    /// 图片是待注入物（走 model.rs 的图片后注，见模块头）。
    pub fn into_tool_output(self) -> ToolOutput {
        ToolOutput::text(self.model_text())
    }

    /// 回执文本；空则退 `error`；再空给固定兜底串（不回显、不 panic）。
    pub fn model_text(&self) -> String {
        if !self.text.is_empty() {
            return self.text.clone();
        }
        if !self.error.is_empty() {
            return self.error.clone();
        }
        "(工具无文本返回)".to_string()
    }

    /// 一行诊断（**stderr 用**）：模型那份回执已经带了原因，这里是给操作者看的——
    /// 「参数错误/未执行」与「执行失败」要分得清，否则现场只看到一句「工具失败」。
    pub fn diagnostic(&self) -> String {
        let mut s = String::new();
        if self.param_error {
            s += "（参数错误，未执行）";
        } else if !self.executed {
            s += "（未执行）";
        }
        s += &format!(": {}", self.model_text());
        // summary 与回执文本不同才补——apply_sql 这类工具的摘要是回执本身。
        if !self.summary.is_empty() && self.summary != self.text {
            s += &format!("（后端摘要: {}）", self.summary);
        }
        s
    }
}

/// 重连参数（2026-09-30 审计 P1：此前连接判死后每个后续调用都 Err 到模型为止，
/// 后端中途重启 = agent 必须重启）。初始连接由 main.rs 建立（含首帧认证），
/// Bridge 只存参数，在连接判死后的**下一次调用**透明重连。
#[derive(Clone)]
pub struct ConnParams {
    /// 后端 AF_UNIX socket 路径
    pub path: String,
    /// `BOT4CJ_MCP_TOKEN`（非空时重连后须重新走首帧认证）
    pub token: Option<String>,
    /// 重连的连接预算（区别于 main.rs 启动期的长预算：中途重连要快失败，
    /// 失败后下一次调用再试，不长时间阻塞当次调用）
    pub connect_timeout: Duration,
    /// 读侧 deadline（透传给重连后的 client，见 frame::HARD_READ_CEILING）
    pub read_timeout: Duration,
}

/// 连接状态机：持一条 lock-step 连接（同一时刻只有一个在途后端调用，正是
/// 物理安全要的语义）；`client = None` = 连接已判死，下一次调用前重连。
struct Conn {
    params: ConnParams,
    client: Option<BackendClient>,
}

impl Conn {
    /// 确保有活连接，返回可调用的 client。重连 = connect（退避重试到
    /// `connect_timeout`）+ 按需首帧认证；重连失败保留判死态，下一次调用
    /// 再试（错误如实上抛，不静默吞掉）。
    async fn ensure(&mut self) -> Result<&mut BackendClient, String> {
        if self.client.is_none() {
            errln!("[mybot-agent] 后端连接已断开，重连: {}", self.params.path);
            let mut c =
                BackendClient::connect_timeout(&self.params.path, self.params.connect_timeout)
                    .await
                    .map_err(|e| format!("后端连接已断开且重连失败: {e}"))?;
            c.set_read_timeout(self.params.read_timeout);
            if let Some(token) = &self.params.token {
                c.authenticate(token)
                    .await
                    .map_err(|e| format!("重连后首帧认证失败: {e}"))?;
            }
            self.client = Some(c);
            errln!("[mybot-agent] 后端重连成功: {}", self.params.path);
        }
        Ok(self.client.as_mut().expect("ensure 刚建立或已有连接"))
    }

    /// 传输失败后判死（在途调用不重试，见 request 的注释）
    fn mark_dead(&mut self) {
        self.client = None;
    }
}

/// 到后端工具层的桥。
pub struct Bridge {
    conn: Arc<Mutex<Conn>>,
    /// 循环层共享状态：任务内预算（admit）与待注入图片队列
    state: Arc<HookState>,
    ica: Option<Arc<StdMutex<IcaLedger>>>,
}

impl Bridge {
    pub fn new(client: BackendClient, params: ConnParams, state: Arc<HookState>) -> Self {
        Self {
            conn: Arc::new(Mutex::new(Conn {
                params,
                client: Some(client),
            })),
            state,
            ica: (std::env::var("BOT4CJ_ICA").as_deref() == Ok("1"))
                .then(|| Arc::new(StdMutex::new(IcaLedger::default()))),
        }
    }

    pub fn ica_enabled(&self) -> bool {
        self.ica.is_some()
    }

    pub fn observe_user(&self, task: &str) {
        if let Some(ica) = &self.ica {
            let mut ledger = ica.lock().unwrap_or_else(crate::hook::poisoned);
            ledger.begin_task(task);
        }
    }

    pub fn ica_note(&self) -> Option<String> {
        self.ica.as_ref().map(|ica| {
            ica.lock()
                .unwrap_or_else(crate::hook::poisoned)
                .final_note()
        })
    }

    pub fn ica_summary(&self) -> Option<String> {
        self.ica
            .as_ref()
            .map(|ica| ica.lock().unwrap_or_else(crate::hook::poisoned).summary())
    }

    pub fn ica_clear(&self) {
        if let Some(ica) = &self.ica {
            *ica.lock().unwrap_or_else(crate::hook::poisoned) = IcaLedger::default();
        }
    }

    pub fn ica_save(&self, session_file: &str) -> Result<(), String> {
        if let Some(ica) = &self.ica {
            ica.lock()
                .unwrap_or_else(crate::hook::poisoned)
                .save(session_file)?;
        }
        Ok(())
    }

    pub fn ica_restore(&self, session_file: &str) -> Result<(), String> {
        if let Some(ica) = &self.ica {
            *ica.lock().unwrap_or_else(crate::hook::poisoned) = IcaLedger::load(session_file)?;
        }
        Ok(())
    }

    /// 发一个 `_route` 请求。传输失败与后端 `success:false` 都折成 `Err(文本)`——
    /// 上层按「工具级错误」回给模型，绝不假装成功。
    ///
    /// 传输失败（I/O/读超时/超限/非法 UTF-8）时连接判死：**本次**调用如实失败，
    /// **在途调用绝不重试**——回执丢失的 `tool.call` 可能已在后端执行，重发即
    /// 重复物理动作；`Conn::ensure` 会在下一次调用前透明重连（2026-09-30 审计
    /// P1：此前判死后每个后续调用都失败，后端重启 = agent 必须跟着重启）。
    /// 后端 `success:false` 是协议正常工作，不动连接。
    async fn request(&self, route: &str, extra: Map<String, Value>) -> Result<Value, String> {
        let mut req = Map::new();
        req.insert("_route".to_string(), Value::String(route.to_string()));
        for (k, v) in extra {
            req.insert(k, v);
        }
        let mut conn = self.conn.lock().await;
        let resp = match conn.ensure().await {
            Ok(client) => client.call(&Value::Object(req)).await,
            Err(e) => return Err(e),
        };
        let resp = match resp {
            Ok(r) => r,
            Err(e) => {
                conn.mark_dead();
                return Err(format!("后端通信失败: {e}"));
            }
        };
        if !resp_ok(&resp) {
            return Err(resp_error(&resp));
        }
        Ok(resp)
    }

    /// 拉工具表（`tool.list`）。
    pub async fn list_tools(&self) -> Result<Vec<ToolSpec>, String> {
        let resp = self.request("tool.list", Map::new()).await?;
        let arr = resp
            .pointer("/data/tools")
            .and_then(Value::as_array)
            .ok_or_else(|| "后端 data.tools 不是数组".to_string())?;
        let mut specs = Vec::with_capacity(arr.len());
        for t in arr {
            let name = t.get("name").and_then(Value::as_str).unwrap_or("");
            if name.is_empty() {
                continue; // 无名工具跳过（不谎报）
            }
            specs.push(ToolSpec {
                name: name.to_string(),
                description: t
                    .get("description")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string(),
                parameters: to_input_schema(t.get("inputSchema")),
            });
        }
        if self.ica.is_some() {
            if crate::ica::specs()
                .iter()
                .any(|i| specs.iter().any(|s| s.name == i.name))
            {
                return Err("后端工具名与 ICA 工具名冲突".into());
            }
            specs.extend(crate::ica::specs());
        }
        Ok(specs)
    }

    /// 取系统提示词素材（`context.get`）。字段校验交给 `prompt::PromptMaterial`。
    pub async fn context_get(&self) -> Result<Value, String> {
        let resp = self.request("context.get", Map::new()).await?;
        Ok(resp.get("data").cloned().unwrap_or(Value::Null))
    }

    /// 调一个工具（`tool.call`）。`Err` = 传输失败或后端整体拒绝；`Ok(ok=false)`
    /// = 工具级失败（回执文本照样给模型看）。
    pub async fn call(&self, name: &str, arguments: Value) -> Result<ToolOutcome, String> {
        if name.starts_with("ica_") {
            let ica = self.ica.as_ref().ok_or("ICA 未启用")?;
            let text = ica
                .lock()
                .unwrap_or_else(crate::hook::poisoned)
                .execute(name, &arguments)?;
            return Ok(ToolOutcome {
                text,
                ok: true,
                executed: true,
                ..Default::default()
            });
        }
        let action_future = if let Some(ica) = &self.ica {
            let ledger = ica.lock().unwrap_or_else(crate::hook::poisoned);
            ledger.authorize_action(name, &arguments)?
        } else {
            String::new()
        };
        let mut extra = Map::new();
        extra.insert("name".to_string(), Value::String(name.to_string()));
        extra.insert("arguments".to_string(), arguments);
        let resp = self.request("tool.call", extra).await?;
        let data = resp.get("data").cloned().unwrap_or(Value::Null);
        let get_str = |k: &str| {
            data.get(k)
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string()
        };
        let mut outcome = ToolOutcome {
            text: get_str("text"),
            image_data_url: get_str("imageDataUrl"),
            ok: data.get("ok").and_then(Value::as_bool).unwrap_or(true),
            error: get_str("error"),
            executed: data
                .get("executed")
                .and_then(Value::as_bool)
                .unwrap_or(true),
            param_error: data
                .get("paramError")
                .and_then(Value::as_bool)
                .unwrap_or(false),
            summary: get_str("summary"),
        };
        if let Some(ica) = &self.ica {
            let source = if outcome.ok && outcome.executed {
                "tool"
            } else {
                "tool_error"
            };
            let id = ica.lock().unwrap_or_else(crate::hook::poisoned).observe(
                source,
                name,
                &outcome.model_text(),
            );
            if !action_future.is_empty() {
                ica.lock()
                    .unwrap_or_else(crate::hook::poisoned)
                    .action_result(&action_future, &id);
            }
            outcome.text.push_str(&format!(
                "\n[ICA observation {id}; source={name}; this is not verified fact]"
            ));
            if !action_future.is_empty() {
                outcome.text.push_str(&format!(
                    "\n[ICA {action_future}] 行动回执已经到达；请用 ica_compare_future 对照预测与观察 {id}，不要把执行成功等同于目标达成。"
                ));
            }
        }
        Ok(outcome)
    }

    /// 任务内预算判定与图片收集（语义见 budget.rs / 模块头）。
    ///
    /// 降级时 `budget.admit` 已把图片清空，故这里只会把**真正进了上下文**的那张
    /// 排进待注入队列。失败的回执同样收图片：C++ 侧的派发漏斗不因 ok=false 丢弃
    /// 图片，模型看到的是一句失败说明加一张画面。
    fn admit_and_collect(&self, outcome: &mut ToolOutcome) {
        let mut text = outcome.text.clone();
        let mut image = outcome.image_data_url.clone();
        {
            let mut budget = self
                .state
                .budget
                .lock()
                .unwrap_or_else(crate::hook::poisoned);
            budget.admit(&mut text, &mut image, outcome.executed);
        }
        outcome.text = text;
        outcome.image_data_url = image;
        if !outcome.image_data_url.is_empty() {
            self.state.push_image(outcome.image_data_url.clone());
        }
    }
}

/// 把一条后端工具描述装配成 rig 动态工具（schema 原样，回调走帧）。
pub fn dynamic_tool(spec: ToolSpec, bridge: &Arc<Bridge>) -> DynamicTool {
    let name = spec.name.clone();
    let bridge = Arc::clone(bridge);
    let tool = PortableDynamicTool::new(
        spec.name,
        spec.description,
        spec.parameters,
        move |args: Value| {
            let bridge = Arc::clone(&bridge);
            let name = name.clone();
            Box::pin(async move {
                match bridge.call(&name, args).await {
                    Ok(mut outcome) => {
                        bridge.admit_and_collect(&mut outcome);
                        if outcome.ok {
                            Ok(outcome.into_tool_output())
                        } else {
                            // 失败回执：文本照样给模型（rig 把 message 同时作为
                            // 模型可见输出），与 C++ 派发漏斗同口径。
                            errln!("[mybot-agent] 工具 {name} 失败{}", outcome.diagnostic());
                            Err(ToolExecutionError::other(outcome.model_text()))
                        }
                    }
                    Err(e) => Err(ToolExecutionError::other(e)),
                }
            })
        },
    );
    DynamicTool::from_portable(tool)
}

/// 从 `{"success":..,"data":..,"error":..}` 里取判定与字段。
pub fn resp_ok(resp: &Value) -> bool {
    resp.get("success")
        .and_then(Value::as_bool)
        .unwrap_or(false)
}

pub fn resp_error(resp: &Value) -> String {
    resp.get("error")
        .and_then(Value::as_str)
        .unwrap_or("后端返回未知错误")
        .to_string()
}

/// 规范 `inputSchema`：后端给的是 JSON Schema object，原样透传；缺失或形态不对时
/// 回落到最小合法 schema（`{"type":"object"}`）。
pub fn to_input_schema(v: Option<&Value>) -> Value {
    match v {
        Some(Value::Object(m)) => Value::Object(m.clone()),
        _ => serde_json::json!({"type": "object"}),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn input_schema_falls_back_to_object() {
        let mut m = Map::new();
        m.insert("type".to_string(), Value::String("object".to_string()));
        let good = Value::Object(m.clone());
        assert_eq!(to_input_schema(Some(&good)), good);
        // 缺失 / 非对象 → 回落 {"type":"object"}
        let fallback = serde_json::json!({"type": "object"});
        assert_eq!(to_input_schema(None), fallback);
        assert_eq!(to_input_schema(Some(&Value::String("x".into()))), fallback);
    }

    #[test]
    fn resp_helpers_read_success_and_error() {
        let ok: Value = serde_json::json!({"success": true, "data": {}});
        assert!(resp_ok(&ok));
        let bad: Value = serde_json::json!({"success": false, "error": "boom"});
        assert!(!resp_ok(&bad));
        assert_eq!(resp_error(&bad), "boom");
        // 缺 error 字段 → 固定兜底串（不回显、不 panic）
        let noerr: Value = serde_json::json!({"success": false});
        assert!(noerr.get("error").is_none());
        assert!(!resp_error(&noerr).is_empty());
    }

    #[test]
    fn outcome_maps_seven_fields_and_model_text() {
        let outcome = ToolOutcome {
            text: "硬件清单：\n  继电器 relay0".to_string(),
            image_data_url: "data:image/png;base64,aGk=".to_string(),
            ok: false,
            error: "设备忙".to_string(),
            executed: true,
            param_error: false,
            summary: "inserted 1 row".to_string(),
        };
        // 失败回执给模型的仍是**回执文本**（不是 error 字段）
        assert_eq!(outcome.clone().model_text(), "硬件清单：\n  继电器 relay0");
        // 文本为空时退 error；两者都空给固定兜底串
        let e_only = ToolOutcome {
            error: "设备忙".to_string(),
            ..Default::default()
        };
        assert_eq!(e_only.model_text(), "设备忙");
        assert_eq!(ToolOutcome::default().model_text(), "(工具无文本返回)");
        // 工具消息只给文本块（图片不当 tool 消息内容——图片是待注入物，批次 C 注入）
        let out = outcome.into_tool_output();
        assert_eq!(out.as_text(), Some("硬件清单：\n  继电器 relay0"));
    }
}

#[cfg(test)]
mod reconnect_tests {
    use super::*;
    use crate::budget::TaskBudget;
    use crate::frame::{decode_len, encode_frame, HEADER_LEN};
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::{UnixListener, UnixStream};

    fn tmpsock(tag: &str) -> std::path::PathBuf {
        let dir =
            std::env::temp_dir().join(format!("mybot_bridge_rc_{}_{}", std::process::id(), tag));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("s.sock")
    }

    fn params(path: &std::path::Path, connect_timeout: Duration) -> ConnParams {
        ConnParams {
            path: path.to_string_lossy().to_string(),
            token: None,
            connect_timeout,
            read_timeout: Duration::from_secs(60),
        }
    }

    fn hook_state() -> Arc<HookState> {
        Arc::new(HookState::new(0, 0, TaskBudget::new(0, 0, 0)))
    }

    /// 假后端协程：读一帧、回一帧固定 tool.list 应答，然后整条连接下线。
    async fn serve_one_then_die(io: UnixStream) {
        let mut io = io;
        let mut hdr = [0u8; HEADER_LEN];
        io.read_exact(&mut hdr).await.unwrap();
        let n = decode_len(&hdr) as usize;
        let mut buf = vec![0u8; n];
        io.read_exact(&mut buf).await.unwrap();
        let resp = serde_json::to_vec(&serde_json::json!({"success": true, "data": {"tools": []}}))
            .unwrap();
        io.write_all(&encode_frame(&resp).unwrap()).await.unwrap();
        io.flush().await.unwrap();
        // 模拟后端重启：连接与监听一起消失
    }

    #[tokio::test]
    async fn bridge_reconnects_transparently_after_connection_death() {
        let path = tmpsock("revive");
        // 第一代后端：接受连接、服务一次 tool.list 后整体下线
        let listener1 = UnixListener::bind(&path).unwrap();
        let client =
            BackendClient::connect_timeout(&path.to_string_lossy(), Duration::from_secs(5))
                .await
                .unwrap();
        let (io1, _) = listener1.accept().await.unwrap();
        let serve = tokio::spawn(serve_one_then_die(io1));

        let bridge = Bridge::new(client, params(&path, Duration::from_secs(5)), hook_state());
        let tools = bridge.list_tools().await.unwrap();
        assert!(tools.is_empty());
        serve.await.unwrap();
        drop(listener1);

        // 连接已死：当次调用如实失败（在途调用绝不重试——回执丢失的
        // tool.call 可能已在后端执行，重发即重复物理动作）
        let err = bridge.list_tools().await.unwrap_err();
        assert!(err.contains("后端通信失败"), "实际: {err}");

        // 第二代后端重新监听（先摘陈旧 socket 文件，与真后端 stale 处理同款）：
        // 下一次调用应透明重连并成功
        std::fs::remove_file(&path).unwrap();
        let listener2 = UnixListener::bind(&path).unwrap();
        let serve2 = tokio::spawn(async move {
            let (io2, _) = listener2.accept().await.unwrap();
            serve_one_then_die(io2).await;
        });
        let tools2 = bridge.list_tools().await.unwrap();
        assert!(tools2.is_empty());
        serve2.await.unwrap();
        let _ = std::fs::remove_dir_all(path.parent().unwrap());
    }

    #[tokio::test]
    async fn conn_ensure_fails_loudly_and_stays_dead_when_backend_is_gone() {
        // 无监听路径：重连在预算内失败，错误点名「重连失败」；判死态保留，
        // 下一次调用再试（不静默、不永久放弃）
        let path = tmpsock("gone");
        let mut conn = Conn {
            params: params(&path, Duration::from_secs(1)),
            client: None,
        };
        let err = match conn.ensure().await {
            Err(e) => e,
            Ok(_) => panic!("后端不存在时应重连失败"),
        };
        assert!(err.contains("重连失败"), "实际: {err}");
        assert!(conn.client.is_none());
        let _ = std::fs::remove_dir_all(path.parent().unwrap());
    }
}
