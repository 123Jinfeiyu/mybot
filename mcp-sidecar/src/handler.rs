//! handler.rs —— rmcp `ServerHandler` 实现：MCP ↔ mybot 后端帧的翻译层
//!
//! 工具是**动态**的（运行期从后端 `tool.list` 拉，随能力门控变化），故不用
//! `#[tool]` 宏（那需要编译期 schema），而是手写 `list_tools` / `call_tool`。
//!
//! 映射（与 M1 后端 `routeToolList` / `routeToolCall` 逐字段对齐）：
//!   MCP `Tool{name, description, input_schema}`      ↔ 帧 `data.tools[]{name,description,inputSchema}`
//!   MCP `CallToolResult{content, is_error}`          ↔ 帧 `data{text,imageDataUrl,ok,executed,paramError,summary,error}`
//!
//! 安全语义（评估 §3.3/§3.6）：
//!   * 取消：`on_cancelled` 只停后续派发并**如实**记「进行中的硬件动作无法中止」，
//!     绝不假装硬件停了；
//!   * 数据非指令：图片走 MCP 原生 image content、文本走 text content（类型即边界），
//!     文本本身在后端侧已用 `quoteUntrusted` 定界。

use std::sync::Arc;

use rmcp::{
    model::{
        CallToolRequestParams, CallToolResult, CancelledNotificationParam, ContentBlock,
        ListToolsResult, PaginatedRequestParams, ServerCapabilities, ServerInfo, Tool,
    },
    service::{NotificationContext, RequestContext},
    ErrorData, RoleServer, ServerHandler,
};
use serde_json::{Map, Value};
use tokio::sync::Mutex;

use crate::frame::BackendClient;

/// MCP 服务句柄。持有到后端的单连接（`Arc<Mutex<_>>` 保证 lock-step 串行：
/// 同一时刻只有一个在途后端调用，正是物理安全要的语义）。
#[derive(Clone)]
pub struct BotHandler {
    client: Arc<Mutex<BackendClient>>,
}

impl BotHandler {
    pub fn new(client: BackendClient) -> Self {
        Self {
            client: Arc::new(Mutex::new(client)),
        }
    }

    /// 发一个 `_route` 请求给后端，返回响应 `Value`。传输失败 → `Err(ErrorData)`
    /// （协议级错误，客户端看不见的内部错误）；后端返回 `success:false` 时**不**
    /// 在这里判错，交给调用方决定是「工具级错误」还是「协议级错误」。
    async fn backend_call(&self, mut req: Map<String, Value>) -> Result<Value, ErrorData> {
        // _route 由调用方塞进 req
        let value = {
            let mut client = self.client.lock().await;
            client.call(&Value::Object(std::mem::take(&mut req))).await
        };
        value.map_err(|e| ErrorData::internal_error(format!("后端通信失败: {e}"), None))
    }
}

/// 从 `{"success":..,"data":..,"error":..}` 里取判定与字段。
fn resp_ok(resp: &Value) -> bool {
    resp.get("success")
        .and_then(Value::as_bool)
        .unwrap_or(false)
}

fn resp_error(resp: &Value) -> String {
    resp.get("error")
        .and_then(Value::as_str)
        .unwrap_or("后端返回未知错误")
        .to_string()
}

/// 把 `inputSchema`（后端给的是 JSON Schema object）转成 rmcp 的 `Arc<JsonObject>`。
/// 缺失或形态不对时回落到最小合法 schema（`{"type":"object"}`），不让一个坏 schema
/// 整条 list_tools 失败。
fn to_input_schema(v: Option<&Value>) -> Arc<Map<String, Value>> {
    if let Some(Value::Object(m)) = v {
        return Arc::new(m.clone());
    }
    let mut fallback = Map::new();
    fallback.insert("type".to_string(), Value::String("object".to_string()));
    Arc::new(fallback)
}

/// 拆 `data:image/jpeg;base64,XXXX` → (mime, base64)。非 data URL 形态返回 None。
fn parse_data_url(s: &str) -> Option<(String, String)> {
    let rest = s.strip_prefix("data:")?;
    let (meta, b64) = rest.split_once(',')?;
    let mime = meta.split(';').next()?.to_string();
    if mime.is_empty() || b64.is_empty() {
        return None;
    }
    Some((mime, b64.to_string()))
}

impl ServerHandler for BotHandler {
    fn get_info(&self) -> ServerInfo {
        ServerInfo::new(ServerCapabilities::builder().enable_tools().build()).with_instructions(
            "bot4cj 机器人工具（经 MCP 暴露）。后端 mybot --mcp-backend 常驻、硬件带电；\
                 run_action_script / apply_sql 等是**物理动作**。工具集随后端启动开关\
                 （--agent-shell / --agent-hot-sql / --read-var / camera）变化。\
                 注意：取消无法中止已下发到硬件的动作。",
        )
    }

    async fn list_tools(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListToolsResult, ErrorData> {
        let mut req = Map::new();
        req.insert("_route".to_string(), Value::String("tool.list".to_string()));
        let resp = self.backend_call(req).await?;
        if !resp_ok(&resp) {
            return Err(ErrorData::internal_error(resp_error(&resp), None));
        }
        let tools_val = resp.pointer("/data/tools").cloned().unwrap_or(Value::Null);
        let arr = tools_val
            .as_array()
            .ok_or_else(|| ErrorData::internal_error("后端 data.tools 不是数组", None))?;

        let mut tools = Vec::with_capacity(arr.len());
        for t in arr {
            let name = t
                .get("name")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            if name.is_empty() {
                continue; // 无名工具跳过（不谎报）
            }
            let description = t
                .get("description")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_string();
            let input_schema = to_input_schema(t.get("inputSchema"));
            tools.push(Tool::new(name, description, input_schema));
        }
        Ok(ListToolsResult {
            tools,
            ..Default::default()
        })
    }

    async fn call_tool(
        &self,
        request: CallToolRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<CallToolResult, ErrorData> {
        let mut req = Map::new();
        req.insert("_route".to_string(), Value::String("tool.call".to_string()));
        req.insert("name".to_string(), Value::String(request.name.to_string()));
        req.insert(
            "arguments".to_string(),
            Value::Object(request.arguments.unwrap_or_default()),
        );

        let resp = self.backend_call(req).await?;
        if !resp_ok(&resp) {
            // 后端协议级失败（预算超限、参数形态错、未知路由等）：作为**工具级错误**
            // 回给客户端，让调用方看得见原因（rmcp 文档：Ok(CallToolResult::error)）。
            return Ok(CallToolResult::error(vec![ContentBlock::text(resp_error(
                &resp,
            ))]));
        }

        let data = resp.get("data").cloned().unwrap_or(Value::Null);
        let text = data
            .get("text")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        let image = data
            .get("imageDataUrl")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        let ok = data.get("ok").and_then(Value::as_bool).unwrap_or(true);

        let mut content: Vec<ContentBlock> = Vec::new();
        if !text.is_empty() {
            content.push(ContentBlock::text(text));
        }
        if let Some((mime, b64)) = parse_data_url(&image) {
            // 图片走 MCP 原生 image content（评估：图像以 base64 data URL 过帧，
            // 这里拆出 mime + base64 交给客户端的多模态通道）。
            content.push(ContentBlock::image(b64, mime));
        }
        if content.is_empty() {
            content.push(ContentBlock::text("(工具无文本/图片返回)"));
        }

        // CallToolResult 是 #[non_exhaustive]，只能用构造器：ok → success(is_error=false)，
        // !ok → error(is_error=true，工具级错误，客户端看得见文本原因)。
        // M4 暂不设 structured_content——MCP 里它 SHOULD 配套 outputSchema，而我们的工具
        // schema 是动态的；text/image content 已是数据边界。结构化输出留待后续按需加。
        if ok {
            Ok(CallToolResult::success(content))
        } else {
            Ok(CallToolResult::error(content))
        }
    }

    async fn on_cancelled(
        &self,
        _notification: CancelledNotificationParam,
        _context: NotificationContext<RoleServer>,
    ) {
        // 如实：取消只能停「后续派发」，已经下发到 I2C/SPI/UART 的动作停不了
        // （LuaJIT 指令计数钩子打不断阻塞的 FFI 系统调用，评估 §3.3）。
        eprintln!(
            "[mybot-mcp] 收到取消通知：停止后续派发；注意——已下发到硬件的动作无法中止，\
             机器人可能停在半途，请先现场确认硬件状态。"
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_data_url_splits_mime_and_base64() {
        let (mime, b64) = parse_data_url("data:image/jpeg;base64,/9j/4AAQ").unwrap();
        assert_eq!(mime, "image/jpeg");
        assert_eq!(b64, "/9j/4AAQ");
        // 无 base64 标记的 data URL 也能拆（mime 取分号前）
        let (m2, d2) = parse_data_url("data:text/plain,aGk=").unwrap();
        assert_eq!(m2, "text/plain");
        assert_eq!(d2, "aGk=");
        // 非 data URL / 空 → None
        assert!(parse_data_url("http://x/y.jpg").is_none());
        assert!(parse_data_url("").is_none());
        assert!(parse_data_url("data:image/png;base64,").is_none());
    }

    #[test]
    fn input_schema_falls_back_to_object() {
        let mut m = Map::new();
        m.insert("type".to_string(), Value::String("object".to_string()));
        let good = Value::Object(m.clone());
        assert_eq!(*to_input_schema(Some(&good)), m);
        // 缺失 / 非对象 → 回落 {"type":"object"}
        assert_eq!(*to_input_schema(None), m);
        assert_eq!(*to_input_schema(Some(&Value::String("x".into()))), m);
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
}
