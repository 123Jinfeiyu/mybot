//! history.rs —— 会话历史的字节预算与整块裁剪（`src/agenthistory.cpp` 的镜像）
//!
//! 裁剪只裁**已完结的旧任务块**，且最新任务块无条件保留：正在进行的对话不能裁空
//! （裁空了任务无法推进），单块超预算也不封顶。块起点在 C++ 侧的判据是
//! 「role == user 且 images 为空」——即「一条不带图片的 user 消息开启一个新任务块」
//! （带图片的 user 消息是同一任务块里的图片后注）。rig 形态等价判据：
//! `Message::User` 且内容里**既没有 ToolResult 也没有 Image**（rig 没有独立的 tool
//! 角色，工具结果装在 `User{content:[ToolResult..]}` 里）。
//!
//! 记账口径（C4，与 C++ 的 messageWeight 同源）：只估算正文/调用参数/图片/**思维链**
//! 内容的字节，不计 JSON 语法开销、role 字符串与非 ASCII 的转义膨胀——作为裁剪判据
//! 方向正确、量级够用。C++ 侧另有两点在 rig 侧天然不同：
//!   * C++ 的 messages[0] 恒为 system（不计入预算、永不裁剪）；rig 的 system 是
//!     agent 的 preamble，根本不在这份历史里，故无「跳过第 0 条」的特例；
//!   * 工具 schema 不在历史里（rig 每次请求单独带 tools），同样不计。

use rig_agent::completion::message::{
    AdditionalParams, DocumentSourceKind, Image, ReasoningContent, ToolResultContent, UserContent,
};
use rig_agent::completion::{AssistantContent, Message};

#[derive(Default)]
pub struct ConversationHistory {
    messages: Vec<Message>,
    written_prefix: usize,
}

impl ConversationHistory {
    pub fn resumed(messages: Vec<Message>) -> Self {
        Self {
            written_prefix: messages.len(),
            messages,
        }
    }

    pub fn snapshot(&self) -> Vec<Message> {
        self.messages.clone()
    }

    pub fn len(&self) -> usize {
        self.messages.len()
    }

    pub fn written_prefix(&self) -> usize {
        self.written_prefix
    }

    pub fn trim(&mut self, budget: usize, log: &mut dyn FnMut(&str)) -> usize {
        let dropped = trim(&mut self.messages, budget, log);
        self.written_prefix = self.written_prefix.saturating_sub(dropped);
        dropped
    }

    pub fn replace(&mut self, messages: Vec<Message>) {
        self.messages = messages;
    }

    pub fn persist(&mut self, mut append: impl FnMut(&Message) -> bool) {
        for message in &self.messages[self.written_prefix..] {
            if !append(message) {
                break;
            }
            self.written_prefix += 1;
        }
    }

    pub fn clear(&mut self) {
        self.messages.clear();
        self.written_prefix = 0;
    }
}

/// 估算一条消息的线上体积（下界）。
pub fn message_weight(m: &Message) -> usize {
    match m {
        Message::System { content } => content.len(),
        Message::User { content } => content.iter().map(user_content_weight).sum(),
        Message::Assistant { id, content } => {
            let mut n = id.as_ref().map_or(0, |s| s.len());
            for item in content {
                n += match item {
                    AssistantContent::Text(t) => t.text.len() + params_len(&t.additional_params),
                    AssistantContent::ToolCall(c) => {
                        c.id.as_str().len()
                            + c.function.name.len()
                            + serde_json::to_string(&c.function.arguments).map_or(0, |s| s.len())
                    }
                    // 思维链必须计入：DeepSeek 的 reasoning_content 随历史逐轮回传，
                    // 它和正文一样占请求体（C++ 的 extras 就是这一项）。
                    AssistantContent::Reasoning(r) => r
                        .content
                        .iter()
                        .map(|c| match c {
                            ReasoningContent::Text { text, signature } => {
                                text.len() + signature.as_ref().map_or(0, |s| s.len())
                            }
                            ReasoningContent::Encrypted(s) => s.len(),
                            ReasoningContent::Redacted { data } => data.len(),
                            ReasoningContent::Summary(s) => s.len(),
                        })
                        .sum::<usize>(),
                    AssistantContent::Image(i) => image_weight(i),
                };
            }
            n
        }
    }
}

fn user_content_weight(c: &UserContent) -> usize {
    match c {
        UserContent::Text(t) => t.text.len() + params_len(&t.additional_params),
        UserContent::ToolResult(r) => {
            let mut n = r.call.as_str().len() + r.name.len();
            for item in &r.content {
                n += match item {
                    ToolResultContent::Text(t) => t.text.len() + params_len(&t.additional_params),
                    ToolResultContent::Json { value } => {
                        serde_json::to_string(value).map_or(0, |s| s.len())
                    }
                    ToolResultContent::Image(i) => image_weight(i),
                };
            }
            n
        }
        UserContent::Image(i) => image_weight(i),
        UserContent::Audio(a) => source_len(&a.data) + params_len(&a.additional_params),
        UserContent::Video(v) => source_len(&v.data) + params_len(&v.additional_params),
        UserContent::Document(d) => source_len(&d.data) + params_len(&d.additional_params),
    }
}

/// 图片体积：base64/data URL 是历史体积的主项（C++ 的 images 数组同口径）。
fn image_weight(i: &Image) -> usize {
    source_len(&i.data) + params_len(&i.additional_params)
}

fn source_len(kind: &DocumentSourceKind) -> usize {
    match kind {
        DocumentSourceKind::Url(s)
        | DocumentSourceKind::Base64(s)
        | DocumentSourceKind::FileId(s)
        | DocumentSourceKind::String(s) => s.len(),
        DocumentSourceKind::Raw(b) => b.len(),
        DocumentSourceKind::Unknown => 0,
    }
}

fn params_len(p: &Option<AdditionalParams>) -> usize {
    p.as_ref()
        .and_then(|p| serde_json::to_string(p).ok())
        .map_or(0, |s| s.len())
}

/// 任务块整块裁剪。返回被裁掉的消息条数（0 = 没裁）。
///
/// 语义与 `HistoryManager::trim` 逐条一致：预算为 0 或不含任务块时不动；从最新块
/// 向旧累计，最新块无条件保留，其余「装得下就留」；被裁的整块（含其中的图片）
/// 一起消失，且裁剪前会经 `log` 如实告知操作者（C++ 侧受 verbose 闸门控制；本进程
/// 一律走 stderr——裁剪意味着更早的任务上下文不再可见，这种事实不该静默）。
pub fn trim(history: &mut Vec<Message>, budget: usize, log: &mut dyn FnMut(&str)) -> usize {
    if budget == 0 || history.is_empty() {
        return 0;
    }
    // 任务块起点：不带工具结果、不带图片的 user 消息
    let starts: Vec<usize> = history
        .iter()
        .enumerate()
        .filter(|(_, m)| match m {
            Message::User { content } => !content
                .iter()
                .any(|c| matches!(c, UserContent::ToolResult(_) | UserContent::Image(_))),
            _ => false,
        })
        .map(|(i, _)| i)
        .collect();
    if starts.is_empty() {
        // 防御：历史理应至少含一次任务（一条不带图片的 user 消息）
        return 0;
    }

    let block_weight: Vec<usize> = starts
        .iter()
        .enumerate()
        .map(|(j, &start)| {
            let end = starts.get(j + 1).copied().unwrap_or(history.len());
            history[start..end].iter().map(message_weight).sum()
        })
        .collect();

    let mut keep = starts.len() - 1;
    let mut acc = block_weight[keep];
    while keep > 0 && acc + block_weight[keep - 1] <= budget {
        keep -= 1;
        acc += block_weight[keep];
    }
    if keep == 0 {
        return 0; // 全部塞得下，无事可做
    }
    let dropped = starts[keep];
    history.drain(0..dropped);
    log(&format!(
        "[历史] 超出字节预算，已裁掉最早的 {dropped} 条消息（含其中的图片）；\
         更早的任务上下文不再可见"
    ));
    dropped
}

#[cfg(test)]
mod tests {
    use super::*;

    /// 造一段「任务块」：user 任务 + assistant(tool_calls) + tool 结果 + 图片后注。
    fn task_block(task: &str, tool_text: &str, image_bytes: usize) -> Vec<Message> {
        let mut msgs = vec![Message::user(task.to_string())];
        msgs.push(Message::Assistant {
            id: None,
            content: vec![AssistantContent::ToolCall(
                rig_agent::completion::message::ToolCall::new(
                    rig_agent::completion::message::ToolCallId::new_or_mint("call_0"),
                    rig_agent::completion::message::ToolFunction::new(
                        "capture_image".to_string(),
                        serde_json::json!({}),
                    ),
                ),
            )],
        });
        msgs.push(Message::User {
            content: vec![UserContent::tool_result(
                "call_0",
                "capture_image",
                vec![ToolResultContent::Text(tool_text.to_string().into())],
            )],
        });
        if image_bytes > 0 {
            msgs.push(Message::User {
                content: vec![
                    UserContent::text("以上工具返回的图片如下，请结合画面内容继续。"),
                    UserContent::image_base64("A".repeat(image_bytes), None, None),
                ],
            });
        }
        msgs
    }

    #[test]
    fn trimming_written_history_does_not_skip_new_messages() {
        let old = task_block("旧任务", "旧结果", 1024);
        let retained = task_block("保留任务", "保留结果", 0);
        let mut initial = old.clone();
        initial.extend(retained.clone());
        let mut history = ConversationHistory::resumed(initial);
        assert_eq!(history.trim(1, &mut |_| {}), old.len());
        assert_eq!(history.written_prefix(), retained.len());

        let new = vec![Message::user("新任务"), Message::assistant("新回答")];
        let mut completed = history.snapshot();
        completed.extend(new.clone());
        history.replace(completed);
        let mut written = Vec::new();
        history.persist(|m| {
            written.push(m.clone());
            true
        });
        assert_eq!(written, new);
        assert_eq!(history.written_prefix(), history.len());
        history.persist(|_| panic!("已落盘消息不得重复追加"));
    }

    #[test]
    fn trimming_unwritten_history_saturates_the_written_prefix() {
        let mut history = ConversationHistory::resumed(task_block("已落盘", "结果", 0));
        let retained = task_block("最新未落盘", "完整回执", 0);
        let mut pending = history.snapshot();
        pending.extend(task_block("较早未落盘", "完整回执", 1024));
        let expected_dropped = pending.len();
        pending.extend(retained.clone());
        history.replace(pending);
        assert_eq!(history.trim(1, &mut |_| {}), expected_dropped);
        assert_eq!(history.written_prefix(), 0);
        let mut written = Vec::new();
        history.persist(|m| {
            written.push(m.clone());
            true
        });
        assert_eq!(written, retained);
    }

    #[test]
    fn completed_rounds_from_failed_task_are_written_with_next_success() {
        let old = task_block("已落盘", "结果", 0);
        let mut history = ConversationHistory::resumed(old.clone());
        let completed_round = task_block("失败任务", "失败前的完整回执", 64);
        let mut boundary = history.snapshot();
        boundary.extend(completed_round.clone());
        history.replace(boundary);
        assert_eq!(history.written_prefix(), old.len());
        assert_eq!(history.trim(0, &mut |_| panic!("不应裁剪")), 0);

        let next = vec![Message::user("下一任务"), Message::assistant("完成")];
        let mut completed = history.snapshot();
        completed.extend(next.clone());
        history.replace(completed);
        let mut written = Vec::new();
        history.persist(|m| {
            written.push(m.clone());
            true
        });
        let mut expected = completed_round;
        expected.extend(next);
        assert_eq!(written, expected);
    }

    #[test]
    fn failed_append_stops_without_advancing_past_the_failure() {
        let mut history = ConversationHistory::default();
        history.replace(vec![
            Message::user("任务"),
            Message::assistant("回答"),
            Message::user("下一任务"),
        ]);
        let mut attempts = 0;
        history.persist(|_| {
            attempts += 1;
            attempts == 1
        });
        assert_eq!(attempts, 2);
        assert_eq!(history.written_prefix(), 1);
        history.persist(|_| false);
        assert_eq!(history.written_prefix(), 1);
    }

    #[test]
    fn clearing_resumed_history_resets_persistence() {
        let mut history = ConversationHistory::resumed(task_block("旧任务", "结果", 0));
        history.persist(|_| panic!("恢复的历史不得重写"));
        history.clear();
        assert_eq!(history.len(), 0);
        assert_eq!(history.written_prefix(), 0);
        let new = vec![Message::user("新任务"), Message::assistant("新回答")];
        history.replace(new.clone());
        let mut written = Vec::new();
        history.persist(|m| {
            written.push(m.clone());
            true
        });
        assert_eq!(written, new);
    }

    #[test]
    fn newest_block_is_kept_and_old_blocks_are_dropped_whole() {
        let mut h = Vec::new();
        h.extend(task_block("任务一", &"x".repeat(100), 0));
        h.extend(task_block("任务二", &"y".repeat(100), 0));
        let newest_len = task_block("任务三", &"z".repeat(1000), 0).len();
        h.extend(task_block("任务三", &"z".repeat(1000), 0));
        assert_eq!(h.len(), 9);

        // 预算只装得下最新块：前两块整块消失（含它们各自的消息）
        let mut logs: Vec<String> = Vec::new();
        let dropped = trim(&mut h, 1001, &mut |t| logs.push(t.to_string()));
        assert_eq!(dropped, 6, "裁掉最早两块（3 条 x 2）");
        assert_eq!(h.len(), newest_len);
        assert_eq!(h[0], Message::user("任务三".to_string()));
        assert_eq!(logs.len(), 1, "裁剪必须如实告知一次");
        assert_eq!(
            logs[0],
            "[历史] 超出字节预算，已裁掉最早的 6 条消息（含其中的图片）；更早的任务上下文不再可见"
        );
    }

    #[test]
    fn all_blocks_fit_or_budget_zero_means_no_trim() {
        let mut h = Vec::new();
        h.extend(task_block("任务一", "短", 0));
        h.extend(task_block("任务二", "短", 0));
        let before = h.clone();
        // 0 = 不裁剪
        assert_eq!(trim(&mut h, 0, &mut |_| panic!("不该有日志")), 0);
        assert_eq!(h, before);
        // 预算够大：全部塞得下
        assert_eq!(trim(&mut h, 1 << 20, &mut |_| panic!("不该有日志")), 0);
        assert_eq!(h, before);
    }

    #[test]
    fn image_notice_does_not_start_a_new_block() {
        // 图片后注（user + Image）不是新任务块：它的字节能让整块一起被裁
        let mut h = Vec::new();
        let with_img = task_block("任务一", "老", 4096);
        h.extend(with_img.clone());
        h.extend(task_block("任务二", "新", 0));
        let dropped = trim(&mut h, 1, &mut |_| {});
        assert_eq!(dropped, with_img.len(), "第一块连同图片后注一起消失");
        assert_eq!(h[0], Message::user("任务二".to_string()));
    }

    #[test]
    fn message_weight_counts_text_calls_and_images() {
        let block = task_block("任务", "工具输出文本", 1000);
        let w_user = message_weight(&block[0]);
        assert_eq!(w_user, "任务".len());
        let w_asst = message_weight(&block[1]);
        assert!(w_asst >= "capture_image".len() + "call_0".len());
        let w_tool = message_weight(&block[2]);
        assert!(w_tool >= "工具输出文本".len() + "capture_image".len());
        let w_img = message_weight(&block[3]);
        assert_eq!(
            w_img,
            "以上工具返回的图片如下，请结合画面内容继续。".len() + 1000
        );
    }
}
