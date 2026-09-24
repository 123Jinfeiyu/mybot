//! mybot-mcp —— bot4cj 的 MCP sidecar（方案 F / 拓扑 F3，仅 F-生态）
//!
//! 对上：MCP over stdio（被任意 MCP 客户端当子进程拉起）。
//! 对下：pymsg 兼容帧 over AF_UNIX，连到 `mybot --mcp-backend`。
//! 本体只做协议翻译，不碰硬件、不含 agent loop、不链接 C++。
//!
//! 用法：
//!   mybot-mcp <后端 socket 路径>          # 位置参数优先
//!   BOT4CJ_MCP_SOCKET=<路径> mybot-mcp    # 或环境变量
//!
//! MCP 客户端配置（stdio）示例：
//!   { "command": "/path/to/mybot-mcp", "args": ["/run/user/1000/bot4cj/mcp.sock"] }

mod frame;
mod handler;

use anyhow::{Context, Result};
use rmcp::{transport::stdio, ServiceExt};

use crate::{frame::BackendClient, handler::BotHandler};

#[tokio::main]
async fn main() -> Result<()> {
    let path = resolve_socket_path()?;

    // 后端可能晚起（上电 + 装结构脚本），connect 内部退避重试直到预算耗尽。
    let client = BackendClient::connect(&path)
        .await
        .with_context(|| format!("无法连接后端 socket: {path}"))?;

    let handler = BotHandler::new(client);
    let service = handler
        .serve(stdio())
        .await
        .map_err(|e| anyhow::anyhow!("MCP serve 初始化失败: {e:?}"))?;

    // 就绪信息走 stderr：stdout 是 MCP 的 JSON-RPC 通道，绝不能混入日志。
    eprintln!("[mybot-mcp] MCP server 就绪（stdio）；后端: {path}");

    // 阻塞直到 stdio 传输 EOF（客户端断开）或内部退出。
    let _reason = service.waiting().await.context("MCP 服务运行异常")?;
    eprintln!("[mybot-mcp] 服务结束（stdio 关闭）");
    Ok(())
}

/// socket 路径来源：位置参数 1 优先，其次环境变量 `BOT4CJ_MCP_SOCKET`。
/// 两者都缺 → 响亮报错退出（不猜、不回落缺省路径）。
fn resolve_socket_path() -> Result<String> {
    if let Some(p) = std::env::args().nth(1) {
        if !p.is_empty() {
            return Ok(p);
        }
    }
    if let Ok(p) = std::env::var("BOT4CJ_MCP_SOCKET") {
        if !p.is_empty() {
            return Ok(p);
        }
    }
    anyhow::bail!("缺少后端 socket 路径：给第一个位置参数，或设环境变量 BOT4CJ_MCP_SOCKET")
}
