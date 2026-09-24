//! model.rs —— 模型装饰器：图片注入 + 单次调用超时 + 网络重试（U-1）
//!
//! rig 没有 completion POST 级重试（`http_client::retry` 是 SSE 事件源用的），
//! 也没有「工具结果之后的图片后注」——两者都在这一层补：
//!
//! 1. **图片注入**：C++ 把工具返回的图片作为 tool 消息**之后**的一条独立 user
//!    消息塞进历史（OpenAI 不允许工具结果里带图片）。rig 侧的等价物是把它追加到
//!    「含工具结果的那条 user 消息」的末尾——provider 转换时，同一条 user 消息里的
//!    ToolResult 先各自成 tool 消息，末尾攒下的 Text/Image 再 flush 成一条 user
//!    消息，于是线上次序正是 `[tool…, user(说明+图片)]`，与 C++ 逐位同序。
//!    注入按「第几条工具结果消息」定位（见 hook::Injection）：本任务内每次请求
//!    都重新注入，任务结束时由 REPL 物化进会话历史。
//!    **注意**：请求里的 prompt 是独立追加在历史之后的一条消息，`RequestPatch.history`
//!    碰不到它——所以图片注入必须在拿到最终请求体的这一层做，而不是钩子里。
//!
//! 2. **超时与重试**：单次调用超时（`--llm-timeout`）是**可重试**类失败；重试分类
//!    只有结构化判据：HTTP 429 = 限流（退避优先采用服务端 Retry-After）、5xx 与
//!    连接类（`http_client::Error::Instance`，含连接被拒/DNS/传输超时）= 瞬时，
//!    其余 4xx（鉴权、配额、请求本身有问题）= 不重试。这与 C++ 的 curl rc 表同口径
//!    （「重试是否可能改变结果」的判据，不是「错误严重不严重」），只是把 curl 退出码
//!    换成了 HTTP 状态码与 reqwest 错误分类——已记档为如实偏差。
//!
//! 3. **审计**：每次模型调用（含重试的尝试次数、请求体估算字节）在这里记一条，
//!    由 REPL 落 model 行。

use std::sync::atomic::Ordering;
use std::sync::Arc;
use std::time::{Duration, Instant};

use rig_agent::completion::message::UserContent;
use rig_agent::completion::{
    AssistantContent, CompletionError, CompletionModel, CompletionRequest, CompletionResponse,
    Message, ProviderCapabilities,
};
// 流式响应类型在 rig-core 的 streaming 模块（completion 只重导出消息与 request 面）
use rig_core::streaming::StreamingCompletionResponse;

use crate::hook::{has_tool_result, CallRecord, HookState, Injection, IMAGE_CAPTION};
use crate::out::errln;

/// 单次退避上限（毫秒）——与 C++ `llm_limits::kRetryBackoffCapMs` 逐值对齐。
pub const RETRY_BACKOFF_CAP_MS: u64 = 60_000;
/// 服务端 Retry-After 的钳制区间（毫秒）——与 C++ `kRetryAfterMinMs/MaxMs` 逐值对齐。
pub const RETRY_AFTER_MIN_MS: u64 = 1_000;
pub const RETRY_AFTER_MAX_MS: u64 = 5 * 60 * 1000;

/// 单次调用超时的错误标记：只有它（而不是所有 RequestError）属于可重试类，
/// 免得「工具表取不到」这类请求构造期错误也被退避重试。
#[derive(Debug)]
struct ModelTimeout {
    seconds: i64,
}

impl std::fmt::Display for ModelTimeout {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "模型调用超时（{} 秒；--llm-timeout / 配置文件的 timeout）",
            self.seconds
        )
    }
}

impl std::error::Error for ModelTimeout {}

#[derive(Debug)]
struct Interrupted;

impl std::fmt::Display for Interrupted {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // 逐字同 C++ agentdriver.cpp 的中断文案
        write!(f, "请求被中断")
    }
}

impl std::error::Error for Interrupted {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RetryClass {
    /// 别再试了：4xx（429 除外）、请求本身有问题
    No,
    /// 稍后再试：连接类失败、5xx、单次调用超时
    Transient,
    /// 被限流：429 —— 退避优先采用服务端给的 Retry-After
    Throttled,
}

struct Classified {
    class: RetryClass,
    retry_after_ms: u64,
    message: String,
}

/// 错误分类（纯函数、不碰网络，可离线穷举）。
fn classify(e: &CompletionError) -> Classified {
    let mut retry_after_ms = 0u64;
    if let Some(headers) = e.provider_response_headers() {
        if let Some(v) = headers.get("retry-after").and_then(|v| v.to_str().ok()) {
            if let Ok(secs) = v.trim().parse::<u64>() {
                retry_after_ms = (secs * 1000).clamp(RETRY_AFTER_MIN_MS, RETRY_AFTER_MAX_MS);
            }
        }
    }
    let class = match e.provider_response_status() {
        // 429 是「限流」而不是普通瞬时错误：退避要尊重服务端的 Retry-After
        Some(s) if s.as_u16() == 429 => RetryClass::Throttled,
        Some(s) if s.is_server_error() => RetryClass::Transient,
        Some(_) => RetryClass::No,
        None => match e {
            // 连接被拒 / DNS / 传输中断 / 传输超时（reqwest 侧）
            CompletionError::HttpError(rig_core::http_client::Error::Instance(_)) => {
                RetryClass::Transient
            }
            CompletionError::RequestError(inner)
                if inner.downcast_ref::<ModelTimeout>().is_some() =>
            {
                RetryClass::Transient
            }
            _ => RetryClass::No,
        },
    };
    Classified {
        class,
        retry_after_ms,
        message: e.to_string(),
    }
}

/// 模型装饰器：把 rig 的内置 provider 模型包一层。
pub struct BudgetModel<M> {
    inner: M,
    state: Arc<HookState>,
    /// 单次调用超时（秒；<=0 = 不限）
    timeout_sec: i64,
    /// 尝试次数**含首次**（= max(1, max_retries)）
    max_attempts: u64,
    retry_backoff_ms: u64,
}

impl<M: CompletionModel> BudgetModel<M> {
    pub fn new(
        inner: M,
        state: Arc<HookState>,
        timeout_sec: i64,
        max_retries: i64,
        retry_backoff_ms: i64,
    ) -> Self {
        Self {
            inner,
            state,
            timeout_sec,
            // 钳到 ≥1：max_retries 是「含首次的尝试次数」而配置文件允许 0（= 不重试），
            // 直接拿 0 当上界会「一次都不发」——「不重试」仍然是发一次（同 C++）。
            max_attempts: std::cmp::max(1, max_retries) as u64,
            retry_backoff_ms: retry_backoff_ms.max(0) as u64,
        }
    }

    /// 把桥收集到的图片注入本次请求（见模块头第 1 条）。
    fn inject_images(&self, request: &mut CompletionRequest) {
        let fresh = self.state.take_pending_images();
        if !fresh.is_empty() {
            let ordinal = request
                .chat_history
                .iter()
                .filter(|m| has_tool_result(m))
                .count();
            if ordinal == 0 {
                // 不静默：图片确实没进上下文，操作者必须看得见
                errln!(
                    "[mybot-agent] 警告: 工具返回了 {} 张图片（共 {} 字节 data URL），\
                     但本次请求里没有可挂载的工具结果消息，本批图片**未**注入对话。",
                    fresh.len(),
                    fresh.iter().map(|s| s.len()).sum::<usize>()
                );
            } else {
                let mut content = Vec::with_capacity(fresh.len() + 1);
                content.push(UserContent::text(IMAGE_CAPTION));
                // 后端给的是 data URL（`data:image/png;base64,...`）：原样交给 provider
                // （OpenAI 与 Anthropic 都接受 data URL 作为图片源），字节不做二次编码。
                for url in fresh {
                    content.push(UserContent::image_url(url, None, None));
                }
                self.state.push_injection(Injection { ordinal, content });
            }
        }
        apply_injections(&mut request.chat_history, &self.state.injections());
    }

    /// 一次尝试：单次调用超时是可重试类失败。
    async fn attempt(
        &self,
        request: CompletionRequest,
    ) -> Result<CompletionResponse, CompletionError> {
        if self.timeout_sec > 0 {
            match tokio::time::timeout(
                Duration::from_secs(self.timeout_sec as u64),
                self.inner.completion(request),
            )
            .await
            {
                Ok(r) => r,
                Err(_) => Err(CompletionError::RequestError(Box::new(ModelTimeout {
                    seconds: self.timeout_sec,
                }))),
            }
        } else {
            self.inner.completion(request).await
        }
    }

    /// 退避并打日志（文案与 C++ `backoffAndLog` 逐字一致）。
    async fn backoff_and_log(&self, c: &Classified, attempt: u64) {
        // 本地指数退避：base × 2^(attempt-1)（attempt 从 1 数起，首次重试就是 base），
        // 单次钳到 kRetryBackoffCapMs —— 与 C++ `backoffAndLog` 同式同钳。
        let shift = attempt.saturating_sub(1).min(20);
        let local_ms = self
            .retry_backoff_ms
            .saturating_mul(1u64 << shift)
            .min(RETRY_BACKOFF_CAP_MS);
        let mut delay_ms = local_ms;
        let mut source = "本地指数退避";
        if c.retry_after_ms > 0 && c.retry_after_ms > delay_ms {
            delay_ms = c.retry_after_ms;
            source = "服务端 Retry-After";
        }
        let what = if c.class == RetryClass::Throttled {
            "限流"
        } else {
            "网络错误"
        };
        errln!(
            "[agent] {what}，{delay_ms}ms 后重试（{attempt}/{attempts}，退避来源: {source}）: {message}",
            attempts = self.max_attempts,
            message = c.message
        );
        tokio::time::sleep(Duration::from_millis(delay_ms)).await;
    }

    fn record(
        &self,
        turn: usize,
        req_bytes: u64,
        resp_bytes: u64,
        attempts: u64,
        started: Instant,
        res: Result<&CompletionResponse, &CompletionError>,
    ) {
        let mut rec = CallRecord {
            turn,
            req_bytes,
            resp_bytes,
            attempts,
            ms: started.elapsed().as_millis().min(i64::MAX as u128) as i64,
            ..Default::default()
        };
        match res {
            Ok(resp) => {
                rec.ok = true;
                // provider 给的用量：**只记不推算**（0 = provider 没给）
                let u = resp.usage;
                rec.prompt_tokens = (u.input_tokens > 0).then_some(u.input_tokens as i64);
                rec.completion_tokens = (u.output_tokens > 0).then_some(u.output_tokens as i64);
            }
            Err(e) => {
                rec.error = e.to_string();
            }
        }
        self.state.record_call(rec);
    }
}

/// 把已注入的批次各自追加到对应序号的工具结果消息末尾。
/// 序号在「本轮请求的历史」上从头数（外层 seed + 本轮消息），与前缀稳定对应。
fn apply_injections(history: &mut [Message], injections: &[Injection]) {
    for inj in injections {
        let mut count = 0usize;
        for m in history.iter_mut() {
            if !has_tool_result(m) {
                continue;
            }
            count += 1;
            if count == inj.ordinal {
                if let Message::User { content } = m {
                    content.extend(inj.content.iter().cloned());
                }
                break;
            }
        }
    }
}

impl<M> CompletionModel for BudgetModel<M>
where
    M: CompletionModel + Send + Sync,
{
    async fn completion(
        &self,
        mut request: CompletionRequest,
    ) -> Result<CompletionResponse, CompletionError> {
        self.inject_images(&mut request);
        // SAN-9：请求体估算字节（线上体积下界口径同 C++；含 preamble/工具 schema）
        let req_bytes = serde_json::to_vec(&request).map_or(0, |v| v.len() as u64);
        let turn = self.state.current_turn.load(Ordering::Relaxed);
        let started = Instant::now();
        let mut attempt = 0u64;
        loop {
            // D4：Ctrl-C 后不再发起新的模型轮
            if self.state.stop.load(Ordering::Relaxed) {
                let e = CompletionError::RequestError(Box::new(Interrupted));
                self.record(turn, req_bytes, 0, attempt.max(1), started, Err(&e));
                return Err(e);
            }
            attempt += 1;
            match self.attempt(request.clone()).await {
                Ok(resp) => {
                    let resp_bytes = response_text_bytes(&resp);
                    self.record(turn, req_bytes, resp_bytes, attempt, started, Ok(&resp));
                    return Ok(resp);
                }
                Err(e) => {
                    let c = classify(&e);
                    if c.class == RetryClass::No || attempt >= self.max_attempts {
                        self.record(turn, req_bytes, 0, attempt, started, Err(&e));
                        return Err(e);
                    }
                    self.backoff_and_log(&c, attempt).await;
                    // 二次中断检查（退避期间可能 Ctrl-C）
                    if self.state.stop.load(Ordering::Relaxed) {
                        let e = CompletionError::RequestError(Box::new(Interrupted));
                        self.record(turn, req_bytes, 0, attempt, started, Err(&e));
                        return Err(e);
                    }
                }
            }
        }
    }

    /// 流式面：本进程不用流式（REPL 只走阻塞面），但 trait 要求实现；
    /// 图片注入与阻塞面一致，重试不做（流式重试的语义与 C++ 无对应物）。
    async fn stream(
        &self,
        mut request: CompletionRequest,
    ) -> Result<StreamingCompletionResponse, CompletionError> {
        self.inject_images(&mut request);
        self.inner.stream(request).await
    }

    fn capabilities(&self) -> ProviderCapabilities {
        self.inner.capabilities()
    }
}

/// 响应里**模型可见文本**的字节数（C++ 记的是 `resp.content.size()`——
/// 只算文本，不算工具调用参数；工具调用轮次的响应文本常为空，此时该字段缺省不写）。
fn response_text_bytes(resp: &CompletionResponse) -> u64 {
    resp.choice
        .iter()
        .map(|c| match c {
            AssistantContent::Text(t) => t.text.len() as u64,
            _ => 0,
        })
        .sum()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::budget::TaskBudget;
    use rig_agent::completion::message::ToolResultContent;

    fn state() -> Arc<HookState> {
        Arc::new(HookState::new(900, 16, TaskBudget::new(0, 0, 0)))
    }

    fn tool_result_msg(id: &str) -> Message {
        Message::User {
            content: vec![UserContent::tool_result(
                id,
                "capture_image",
                vec![ToolResultContent::Text("ok".to_string().into())],
            )],
        }
    }

    /// 图片必须追加到「含工具结果的那条消息」末尾：provider 转换后才会落在
    /// tool 消息之后（与 C++ 的图片后注同序）。
    #[test]
    fn images_are_appended_to_the_tool_result_message() {
        let st = state();
        // 上一轮的工具结果 + 本轮的 prompt（rig 的 prompt 是独立追加的最后一条）
        let mut history = vec![
            Message::user("任务一"),
            tool_result_msg("call_0"),
            Message::user("任务二"),
        ];
        st.push_image("data:image/png;base64,AAAA".to_string());
        let fresh = st.take_pending_images();
        let mut content = vec![UserContent::text(IMAGE_CAPTION)];
        for url in fresh {
            content.push(UserContent::image_url(url, None, None));
        }
        st.push_injection(Injection {
            ordinal: 1,
            content,
        });
        apply_injections(&mut history, &st.injections());

        let Message::User { content } = &history[1] else {
            panic!("第 1 条是工具结果消息");
        };
        assert_eq!(content.len(), 3, "工具结果 + 说明 + 图片");
        assert!(matches!(content[0], UserContent::ToolResult(_)));
        assert!(matches!(&content[1], UserContent::Text(t) if t.text == IMAGE_CAPTION));
        assert!(matches!(&content[2], UserContent::Image(_)));
        // 本轮 prompt（任务二）不受影响：图片挂在工具结果那条上，不会插到 prompt 之前
        assert_eq!(history[2], Message::user("任务二"));
    }

    #[test]
    fn injections_land_on_their_own_round_not_the_newest() {
        let history_src = vec![
            Message::user("任务一"),
            tool_result_msg("call_0"),
            Message::assistant("一"),
            Message::user("任务二"),
            tool_result_msg("call_1"),
            Message::user("任务三"),
        ];
        let injections = vec![
            Injection {
                ordinal: 2,
                content: vec![UserContent::text("批二")],
            },
            Injection {
                ordinal: 1,
                content: vec![UserContent::text("批一")],
            },
        ];
        let mut history = history_src;
        apply_injections(&mut history, &injections);
        // 注入是「追加到原消息里」，不新增消息：条数与次序都不变
        assert_eq!(history.len(), 6);
        assert!(matches!(history[2], Message::Assistant { .. }));
        assert!(matches!(&history[1], Message::User { content }
            if matches!(&content[1], UserContent::Text(t) if t.text == "批一")));
        assert!(matches!(&history[4], Message::User { content }
            if matches!(&content[1], UserContent::Text(t) if t.text == "批二")));
        // 本轮的 prompt（任务三）不动：图片只挂在各自那轮的工具结果上
        assert_eq!(history[5], Message::user("任务三"));
    }

    #[test]
    fn retry_classification_follows_status_then_transport() {
        use rig_core::http_client;
        // 429 → 限流；退避采用 Retry-After
        let mut headers = http_client::HeaderMap::new();
        headers.insert("retry-after", "30".parse().unwrap());
        let e = CompletionError::from_http_response(
            http::StatusCode::TOO_MANY_REQUESTS,
            "rate limited",
        )
        .with_response_headers(Some(Box::new(headers)));
        let c = classify(&e);
        assert_eq!(c.class, RetryClass::Throttled);
        assert_eq!(c.retry_after_ms, 30_000);
        // 5xx → 瞬时；429 以外的 4xx → 不重试（与 C++ 同口径：重试改变不了结果）
        for (status, want) in [
            (
                http::StatusCode::INTERNAL_SERVER_ERROR,
                RetryClass::Transient,
            ),
            (http::StatusCode::BAD_GATEWAY, RetryClass::Transient),
            (http::StatusCode::UNAUTHORIZED, RetryClass::No),
            (http::StatusCode::BAD_REQUEST, RetryClass::No),
        ] {
            assert_eq!(
                classify(&CompletionError::from_http_response(status, "x")).class,
                want,
                "状态码 {status} 判类错"
            );
        }
        // 连接类（reqwest 错误）→ 瞬时
        let io =
            http_client::Error::Instance(Box::new(std::io::Error::other("connection refused")));
        assert_eq!(
            classify(&CompletionError::HttpError(io)).class,
            RetryClass::Transient
        );
        // 单次调用超时 → 瞬时；其余请求构造错误 → 不重试
        assert_eq!(
            classify(&CompletionError::RequestError(Box::new(ModelTimeout {
                seconds: 1
            })))
            .class,
            RetryClass::Transient
        );
        assert_eq!(
            classify(&CompletionError::RequestError("工具表取不到".into())).class,
            RetryClass::No
        );
        assert_eq!(
            classify(&CompletionError::ProviderError("boom".to_string())).class,
            RetryClass::No
        );
    }

    #[test]
    fn retry_after_below_minimum_is_clamped() {
        use rig_core::http_client;
        let mut headers = http_client::HeaderMap::new();
        headers.insert("retry-after", "0".parse().unwrap());
        let e = CompletionError::from_http_response(http::StatusCode::TOO_MANY_REQUESTS, "x")
            .with_response_headers(Some(Box::new(headers)));
        assert_eq!(classify(&e).retry_after_ms, RETRY_AFTER_MIN_MS);
        // 天文数字被钳到上限（坏网关不能把 agent 挂死一天）
        let mut headers = http_client::HeaderMap::new();
        headers.insert("retry-after", "999999".parse().unwrap());
        let e = CompletionError::from_http_response(http::StatusCode::TOO_MANY_REQUESTS, "x")
            .with_response_headers(Some(Box::new(headers)));
        assert_eq!(classify(&e).retry_after_ms, RETRY_AFTER_MAX_MS);
    }

    #[test]
    fn response_text_bytes_counts_only_text() {
        let resp = CompletionResponse::new(
            vec![
                AssistantContent::text("你好"),
                AssistantContent::ToolCall(rig_agent::completion::message::ToolCall::new(
                    rig_agent::completion::message::ToolCallId::new_or_mint("call_0"),
                    rig_agent::completion::message::ToolFunction::new(
                        "t".to_string(),
                        serde_json::json!({"a": 1}),
                    ),
                )),
            ],
            rig_agent::completion::Usage::new(),
            "test",
        );
        assert_eq!(response_text_bytes(&resp), "你好".len() as u64);
    }
}
