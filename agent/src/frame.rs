//! frame.rs —— mybot 后端的帧客户端（M1 `src/mcpbackend.cpp` 的镜像）
//!
//! 线格式与 pymsg/gomsg/libmsg 字节兼容：`[4 字节 big-endian uint32 载荷长][UTF-8 JSON]`。
//! 上限收紧到 **8 MiB**（对齐后端与 bot4cj JSON 解析器），在**分配载荷缓冲之前**
//! 检查长度（评估 §7.1）。单连接、lock-step、单一在途调用——故 `call` 取 `&mut self`，
//! 上层用 `Mutex` 串行化即可（正是物理安全要的语义）。
//!
//! 只做「一发一收」：不实现 libmsg 那些用不上的机制（线程池、broadcast、
//! retry_on_refused 的 1s 轮询、AsyncQueue），但**照抄它的加固清单**：分配前查上限、
//! 接收侧校验 UTF-8、超限即断。

use std::time::Duration;

use crate::out::errln;
use anyhow::{anyhow, bail, Context, Result};
use serde_json::Value;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::time::{sleep, Instant};

/// 帧头长度：4 字节 big-endian uint32。
pub const HEADER_LEN: usize = 4;

/// 帧载荷上限：8 MiB——与后端 `src/mcpbackend.hpp` 的 `kMcpMaxFrameBytes` 是同一
/// 契约的两份常量（对齐 jsonparse 的 kJsonMaxBytes），改动必须双侧同步。
/// 跨语言一致由 C++ 侧 test_mcpbackend 的 mcpbackend_frame_limit_matches_rust_twin
/// 读本文件比对钉住（改本常量会红那条用例）。
pub const MAX_FRAME_BYTES: u32 = 8 * 1024 * 1024;

/// 把载荷封成一帧：4 字节 BE 长度前缀 + 载荷。纯函数，便于单测。
pub fn encode_frame(payload: &[u8]) -> Result<Vec<u8>> {
    let len = payload.len();
    if len > MAX_FRAME_BYTES as usize {
        bail!("帧载荷 {len} 字节超过上限 {MAX_FRAME_BYTES}");
    }
    let mut frame = Vec::with_capacity(HEADER_LEN + len);
    frame.extend_from_slice(&(len as u32).to_be_bytes());
    frame.extend_from_slice(payload);
    Ok(frame)
}

/// 解析 4 字节帧头为载荷长度（big-endian）。纯函数，便于单测。
pub fn decode_len(hdr: &[u8; HEADER_LEN]) -> u32 {
    u32::from_be_bytes(*hdr)
}

/// 到 mybot 后端的单连接客户端。
pub struct BackendClient {
    stream: UnixStream,
}

impl BackendClient {
    /// 连接后端 AF_UNIX socket。后端可能晚起（上电、装结构脚本），故对连接失败
    /// 退避重试直到 `timeout`；超时则报错（不静默、不永久挂起）。
    pub async fn connect_timeout(path: &str, timeout: Duration) -> Result<Self> {
        let deadline = Instant::now() + timeout;
        let mut delay = Duration::from_millis(250);
        loop {
            match UnixStream::connect(path).await {
                Ok(stream) => return Ok(Self { stream }),
                Err(e) => {
                    if Instant::now() >= deadline {
                        return Err(anyhow!(
                            "连接后端超时（{}s），最后错误: {e}；路径 {path}",
                            timeout.as_secs()
                        ));
                    }
                    errln!(
                        "[mybot-agent] 后端未就绪（{e}），{}ms 后重试: {path}",
                        delay.as_millis()
                    );
                    sleep(delay).await;
                    if delay < Duration::from_secs(4) {
                        delay *= 2;
                    }
                }
            }
        }
    }

    /// 从已建立的流构造（**测试缝**：单测用 `UnixStream::pair()` 注入假后端）。
    #[cfg(test)]
    pub fn from_stream(stream: UnixStream) -> Self {
        Self { stream }
    }

    /// 发一个请求帧、收一个响应帧（lock-step）。任何协议违规（超限、非法 UTF-8、
    /// I/O 错误）都返回 Err——上层据此把连接判死（断连即 resync，评估 §7.1）。
    pub async fn call(&mut self, req: &Value) -> Result<Value> {
        let payload = serde_json::to_vec(req).context("序列化请求")?;
        let frame = encode_frame(&payload)?;
        self.stream.write_all(&frame).await.context("写请求帧")?;
        self.stream.flush().await.context("flush 请求帧")?;

        let mut hdr = [0u8; HEADER_LEN];
        self.stream
            .read_exact(&mut hdr)
            .await
            .context("读响应帧头")?;
        let rlen = decode_len(&hdr);
        // 上限在**分配之前**检查（评估 §7.1）：u32 直接比较，无溢出暴露面。
        if rlen > MAX_FRAME_BYTES {
            bail!("响应帧 {rlen} 字节超过上限 {MAX_FRAME_BYTES}");
        }
        let mut buf = vec![0u8; rlen as usize];
        self.stream
            .read_exact(&mut buf)
            .await
            .context("读响应载荷")?;
        let text = String::from_utf8(buf).map_err(|_| anyhow!("响应帧不是合法 UTF-8"))?;
        let value: Value = serde_json::from_str(&text).context("解析响应 JSON")?;
        Ok(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn encode_frame_is_4byte_be_plus_payload() {
        let f = encode_frame(b"abc").unwrap();
        assert_eq!(f, vec![0, 0, 0, 3, b'a', b'b', b'c']);
        // 空载荷合法（libmsg 口径）
        assert_eq!(encode_frame(b"").unwrap(), vec![0, 0, 0, 0]);
        // 300 = 0x0000012C：多字节 BE 正确
        let big = encode_frame(&vec![b'x'; 300]).unwrap();
        assert_eq!(big.len(), 304);
        assert_eq!(&big[0..4], &[0, 0, 0x01, 0x2C]);
    }

    #[test]
    fn decode_len_roundtrips() {
        assert_eq!(decode_len(&[0, 0, 0x01, 0x2C]), 300);
        assert_eq!(decode_len(&[0xFF, 0xFF, 0xFF, 0xFF]), u32::MAX);
    }

    #[tokio::test]
    async fn call_roundtrips_a_frame() {
        // 假后端：读一帧、回一帧 {"success":true,"data":{"pong":true}}
        let (client_io, mut server_io) = UnixStream::pair().unwrap();
        let mut client = BackendClient::from_stream(client_io);

        let server = tokio::spawn(async move {
            let mut hdr = [0u8; 4];
            server_io.read_exact(&mut hdr).await.unwrap();
            let n = u32::from_be_bytes(hdr) as usize;
            let mut buf = vec![0u8; n];
            server_io.read_exact(&mut buf).await.unwrap();
            let req: Value = serde_json::from_slice(&buf).unwrap();
            assert_eq!(req["_route"], "ping");
            let resp =
                serde_json::to_vec(&json!({"success": true, "data": {"pong": true}})).unwrap();
            server_io
                .write_all(&encode_frame(&resp).unwrap())
                .await
                .unwrap();
        });

        let resp = client.call(&json!({"_route": "ping"})).await.unwrap();
        assert_eq!(resp["success"], true);
        assert_eq!(resp["data"]["pong"], true);
        server.await.unwrap();
    }

    #[tokio::test]
    async fn call_rejects_oversized_response_header() {
        let (client_io, mut server_io) = UnixStream::pair().unwrap();
        let mut client = BackendClient::from_stream(client_io);
        let server = tokio::spawn(async move {
            // 声称载荷 = MAX+1，但根本不发那么多——客户端应在**分配前**拒收
            let bad = (MAX_FRAME_BYTES + 1).to_be_bytes();
            server_io.write_all(&bad).await.unwrap();
        });
        let err = client.call(&json!({"_route": "ping"})).await.unwrap_err();
        assert!(err.to_string().contains("超过上限"), "实际: {err}");
        let _ = server.await;
    }

    #[tokio::test]
    async fn call_rejects_invalid_utf8_response() {
        let (client_io, mut server_io) = UnixStream::pair().unwrap();
        let mut client = BackendClient::from_stream(client_io);
        let server = tokio::spawn(async move {
            let payload: Vec<u8> = vec![0xFF, 0xFE, 0xFD]; // 非法 UTF-8
            server_io
                .write_all(&encode_frame(&payload).unwrap())
                .await
                .unwrap();
        });
        let err = client.call(&json!({"_route": "ping"})).await.unwrap_err();
        assert!(err.to_string().contains("UTF-8"), "实际: {err}");
        let _ = server.await;
    }
}
