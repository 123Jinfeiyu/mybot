//! hook.rs —— 循环层的钩子状态与 rig 钩子实现（C++ agentloop.cpp 的循环语义落点）
//!
//! C++ 的循环语义散在 `AgentSession::runTurn` / `executeToolRound` 里：轮循环开头的
//! 中断与墙钟判定、逐条派发前的墙钟判定、单轮调用上限、失败回滚边界；rig 侧这些都
//! 走 `AgentHook`（见 `.tmp/rig-migration-matrix.md` 的「批次 C 落点设计」）。
//!
//! 本文件只放**判定与记账**；桥（bridge.rs）在工具回执到手时做任务内预算降级，
//! 模型装饰器（model.rs）负责图片注入与重试，REPL（repl.rs）负责裁剪、物化与回滚。
//!
//! 两处墙钟判定点（C++ 也有两处，语义不同）：
//!   * 判定点①（`on_completion_call`）：预算已耗尽就**不再发起新的模型调用**，
//!     以失败收场并如实说明「此前已执行的动作不会回退」；
//!   * 判定点②（`on_tool_call`，逐条派发之前）：轮内耗尽时剩下的调用**不派发**，
//!     但每条都补一条回执——说清「没下发、也没有副作用」，不是执行失败。
//!
//! 单轮工具调用上限：C++ 把超限的调用从 assistant.tool_calls 里**剔除**并把截断
//! 说明追加进 assistant 文本；rig 侧等价物是 `ToolCallAction::Skip`（不派发、模型
//! 看到「未派发」回执）。不派发语义一致，呈现位置不同——已记档为如实偏差。

use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Mutex;

use rig_agent::agent::{
    AgentHook, CompletionCallAction, CompletionCallEvent, HookContext, ToolCall, ToolCallAction,
};
use rig_agent::completion::message::UserContent;
use rig_agent::completion::Message;

use crate::budget::TaskBudget;
use crate::out::errln;

/// 图片后注的说明文字（逐字同 C++ `executeToolRound` 里那条 user 消息）。
pub const IMAGE_CAPTION: &str = "以上工具返回的图片如下，请结合画面内容继续。";

/// 取消文案（逐字同 C++ `runTurn` 与 `agentdriver.cpp` 的「执行被中断」口径）。
pub const STOP_TEXT: &str = "执行被中断";

/// 一次模型调用的观测值（供 REPL 落审计的 model 行）。
#[derive(Debug, Clone, Default)]
pub struct CallRecord {
    /// rig 的 1-based 模型调用序号（写审计时减一，对齐 C++ 的 0-based turn）
    pub turn: usize,
    /// 请求体估算字节（线上体积下界，口径同 C++ SAN-9）
    pub req_bytes: u64,
    /// 模型可见文本字节（C++ 记的是 `resp.content.size()`）
    pub resp_bytes: u64,
    /// 实际尝试次数（>1 = 发生过重试）
    pub attempts: u64,
    pub ms: i64,
    pub ok: bool,
    pub error: String,
    pub prompt_tokens: Option<i64>,
    pub completion_tokens: Option<i64>,
}

/// 一批待注入的图片，以及它应当落在哪条工具结果消息之后。
///
/// `ordinal` 是「本次请求里第几条带工具结果的 user 消息」（1-based，从头数）。
/// 记序号而不是下标：同一个任务里每轮的请求都是重新拼出来的（外层 seed + 本轮消息），
/// 序号在任意前缀上都稳定。
#[derive(Debug, Clone)]
pub struct Injection {
    pub ordinal: usize,
    /// 该批次的 user 消息内容（说明文字 + 若干图片）
    pub content: Vec<UserContent>,
}

/// 本轮已发出的工具调用计数（单轮上限用；turn 变化即重置）。
#[derive(Debug, Clone, Copy, Default)]
struct RoundCalls {
    turn: usize,
    calls: usize,
}

/// 钩子、桥、模型装饰器、REPL 共享的运行期状态。
pub struct HookState {
    pub max_task_seconds: i64,
    /// 0 = 不限
    pub max_calls_per_round: i64,
    /// SIGINT/SIGTERM 置位（一次置位即锁存：取消是操作者的意思，不该被下一轮任务清掉）
    pub stop: AtomicBool,
    pub budget: Mutex<TaskBudget>,
    /// 当前模型调用序号（1-based）——模型装饰器记审计用
    pub current_turn: AtomicUsize,
    round: Mutex<RoundCalls>,
    /// 本轮的「[上限]」stderr 行只打一次
    round_capped: AtomicBool,
    /// 失败回滚边界（rig 的 CompletionError 不带历史，必须自记）
    rollback: Mutex<Option<Vec<Message>>>,
    calls: Mutex<Vec<CallRecord>>,
    /// 桥收集到的、尚未注入的图片（data URL）
    pending_images: Mutex<Vec<String>>,
    /// 已注入的批次（供 REPL 在任务结束时物化进会话历史）
    injections: Mutex<Vec<Injection>>,
}

impl HookState {
    pub fn new(max_task_seconds: i64, max_calls_per_round: i64, budget: TaskBudget) -> Self {
        Self {
            max_task_seconds,
            max_calls_per_round,
            stop: AtomicBool::new(false),
            budget: Mutex::new(budget),
            current_turn: AtomicUsize::new(0),
            round: Mutex::new(RoundCalls::default()),
            round_capped: AtomicBool::new(false),
            rollback: Mutex::new(None),
            calls: Mutex::new(Vec::new()),
            pending_images: Mutex::new(Vec::new()),
            injections: Mutex::new(Vec::new()),
        }
    }

    /// 一轮任务开始：任务内预算清零（含墙钟起点）、清空上一轮留下的观测值与图片。
    /// **不**清 stop —— 取消是锁存的。
    pub fn begin_task(&self) {
        self.budget.lock().unwrap_or_else(poisoned).reset_task();
        *self.round.lock().unwrap_or_else(poisoned) = RoundCalls::default();
        self.round_capped.store(false, Ordering::Relaxed);
        *self.rollback.lock().unwrap_or_else(poisoned) = None;
        self.calls.lock().unwrap_or_else(poisoned).clear();
        self.pending_images.lock().unwrap_or_else(poisoned).clear();
        self.injections.lock().unwrap_or_else(poisoned).clear();
    }

    /// 取走本轮全部模型调用记录（审计落行用）。
    pub fn drain_calls(&self) -> Vec<CallRecord> {
        std::mem::take(&mut *self.calls.lock().unwrap_or_else(poisoned))
    }

    pub fn record_call(&self, rec: CallRecord) {
        self.calls.lock().unwrap_or_else(poisoned).push(rec);
    }

    /// 失败的**回滚边界**：已经完整跑完的轮（含 tool_calls 与其工具回执）。
    /// `None` = 还没发生任何模型调用（此时回滚到任务开始的历史）。
    pub fn rollback_boundary(&self) -> Option<Vec<Message>> {
        self.rollback.lock().unwrap_or_else(poisoned).clone()
    }

    /// 桥收下一张待注入的图片（data URL）。
    pub fn push_image(&self, data_url: String) {
        self.pending_images
            .lock()
            .unwrap_or_else(poisoned)
            .push(data_url);
    }

    /// 取走并物化「本任务已注入的图片批次」——任务结束时由 REPL 调用。
    pub fn take_injections(&self) -> Vec<Injection> {
        std::mem::take(&mut *self.injections.lock().unwrap_or_else(poisoned))
    }

    /// 取走桥收集到、尚未注入的图片（模型装饰器在每次请求前调用）。
    pub fn take_pending_images(&self) -> Vec<String> {
        std::mem::take(&mut *self.pending_images.lock().unwrap_or_else(poisoned))
    }

    /// 记下一个已注入的图片批次（含它挂在哪条工具结果之后）。
    pub fn push_injection(&self, inj: Injection) {
        self.injections.lock().unwrap_or_else(poisoned).push(inj);
    }

    /// 当前已注入的批次快照（每次请求都要重新注入：请求是从外层历史重新拼的）。
    pub fn injections(&self) -> Vec<Injection> {
        self.injections.lock().unwrap_or_else(poisoned).clone()
    }
}

fn poisoned<T>(e: std::sync::PoisonError<T>) -> T {
    e.into_inner()
}

/// 消息里有没有工具结果（rig 把工具回执装在 `User{content:[ToolResult..]}` 里）。
pub fn has_tool_result(m: &Message) -> bool {
    match m {
        Message::User { content } => content
            .iter()
            .any(|c| matches!(c, UserContent::ToolResult(_))),
        _ => false,
    }
}

/// 把已注入的图片批次物化进一段历史（任务结束后调用）：每个批次插到它对应的
/// 工具结果消息**之后**，与 C++「tool 消息之后另起一条 user 图片消息」逐位同序。
pub fn materialize(mut msgs: Vec<Message>, injections: &[Injection]) -> Vec<Message> {
    let mut ordered: Vec<&Injection> = injections.iter().collect();
    ordered.sort_by_key(|i| i.ordinal);
    for inj in ordered {
        let mut count = 0usize;
        let mut at = None;
        for (idx, m) in msgs.iter().enumerate() {
            if has_tool_result(m) {
                count += 1;
                if count == inj.ordinal {
                    at = Some(idx);
                    break;
                }
            }
        }
        match at {
            Some(idx) => msgs.insert(
                idx + 1,
                Message::User {
                    content: inj.content.clone(),
                },
            ),
            None => errln!(
                "[mybot-agent] 警告: 图片批次（第 {} 条工具结果之后）找不到落点，未并入会话历史；\
                 该批次在本次任务里已按请求内注注入给模型",
                inj.ordinal
            ),
        }
    }
    msgs
}

/// 墙钟判定点①的文案（逐字同 C++）。
fn wall_clock_turn_text(seconds: i64) -> String {
    format!(
        "达到单任务墙钟预算（{seconds} 秒）仍未得到最终回答；此前已执行的动作不会回退。\
         可调大 --llm-task-timeout（或配置文件的 max_task_seconds），或把任务拆小"
    )
}

/// 墙钟判定点②的逐条回执文案（逐字同 C++）。
fn wall_clock_call_text(seconds: i64) -> String {
    format!(
        "未派发：单任务墙钟预算（{seconds} 秒）已耗尽，本条调用没有下发\
         （不是执行失败，也没有产生任何副作用）。"
    )
}

/// 循环层钩子。
pub struct SessionHook {
    state: std::sync::Arc<HookState>,
}

impl SessionHook {
    pub fn new(state: std::sync::Arc<HookState>) -> Self {
        Self { state }
    }
}

impl AgentHook for SessionHook {
    /// 判定点① + 回滚边界（见模块头）。
    async fn on_completion_call(
        &self,
        _ctx: &HookContext,
        event: CompletionCallEvent<'_>,
    ) -> CompletionCallAction {
        // D4：Ctrl-C 之后不再发起新的模型轮，让主流程尽快走清理链
        if self.state.stop.load(Ordering::Relaxed) {
            return CompletionCallAction::Stop(STOP_TEXT.to_string());
        }
        let exhausted = self
            .state
            .budget
            .lock()
            .unwrap_or_else(poisoned)
            .exhausted();
        if exhausted {
            return CompletionCallAction::Stop(wall_clock_turn_text(self.state.max_task_seconds));
        }
        // 回滚边界：turn 1 时 history 恰是任务开始前的历史（本轮 user 任务还只是
        // prompt）→ 失败要连它一起丢掉，与 C++ 的 `complete = base` 同口径；
        // turn ≥ 2 时 history + prompt 是一次完整轮（assistant.tool_calls + 全部
        // tool 回执）——硬件动作已经发生，回滚不该假装什么都没做。
        let boundary = if event.turn <= 1 {
            event.history.to_vec()
        } else {
            let mut v = event.history.to_vec();
            v.push(event.prompt.clone());
            v
        };
        *self.state.rollback.lock().unwrap_or_else(poisoned) = Some(boundary);
        self.state.current_turn.store(event.turn, Ordering::Relaxed);
        CompletionCallAction::Continue
    }

    /// 判定点② + 单轮调用上限。
    async fn on_tool_call(&self, ctx: &HookContext, _event: ToolCall<'_>) -> ToolCallAction {
        // 单轮计数：turn 一变就是新一轮（模型的新响应），计数从零起
        let turn = ctx.turn();
        let nth = {
            let mut round = self.state.round.lock().unwrap_or_else(poisoned);
            if round.turn != turn {
                round.turn = turn;
                round.calls = 0;
                self.state.round_capped.store(false, Ordering::Relaxed);
            }
            round.calls += 1;
            round.calls
        };

        if self.state.stop.load(Ordering::Relaxed) {
            return ToolCallAction::Skip(STOP_TEXT.to_string());
        }
        let exhausted = self
            .state
            .budget
            .lock()
            .unwrap_or_else(poisoned)
            .exhausted();
        if exhausted {
            return ToolCallAction::Skip(wall_clock_call_text(self.state.max_task_seconds));
        }
        // 单轮上限：超出的调用**不派发**（不给硬件一次性下发失控数量的动作），
        // 但必须如实告知模型本条没有被下发。C++ 侧这条说明挂在 assistant 文本里
        // （见 agentloop.cpp 的 excess 分支），rig 侧挂在回执上——呈现位置不同、
        // 「未派发」语义一致。
        if self.state.max_calls_per_round > 0 && nth > self.state.max_calls_per_round as usize {
            if !self.state.round_capped.swap(true, Ordering::Relaxed) {
                errln!(
                    "[上限] 本轮工具调用超过单轮上限 {} 条，超出部分未派发\
                     （模型收到「未派发」回执，不是执行失败）",
                    self.state.max_calls_per_round
                );
            }
            return ToolCallAction::Skip(format!(
                "未派发：本轮工具调用数超过单轮上限（{} 条），本条调用没有下发\
                 （不是执行失败，也没有产生任何副作用）。",
                self.state.max_calls_per_round
            ));
        }
        ToolCallAction::Run
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use rig_agent::completion::message::ToolResultContent;

    fn tool_result_msg(id: &str) -> Message {
        Message::User {
            content: vec![UserContent::tool_result(
                id,
                "capture_image",
                vec![ToolResultContent::Text("ok".to_string().into())],
            )],
        }
    }

    #[test]
    fn materialize_inserts_each_batch_after_its_own_tool_result() {
        let msgs = vec![
            Message::user("任务一"),
            tool_result_msg("call_0"),
            Message::assistant("一做完"),
            Message::user("任务二"),
            tool_result_msg("call_1"),
            Message::assistant("二做完"),
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
        let out = materialize(msgs, &injections);
        assert_eq!(out.len(), 8);
        // 批一跟在第一条工具结果之后；批二跟在第二条之后（各自归位，互不串位）
        assert_eq!(out[2], Message::user("批一"));
        assert_eq!(out[3], Message::assistant("一做完"));
        assert_eq!(out[6], Message::user("批二"));
        assert_eq!(out[7], Message::assistant("二做完"));
    }

    #[test]
    fn has_tool_result_distinguishes_plain_user_messages() {
        assert!(has_tool_result(&tool_result_msg("call_0")));
        assert!(!has_tool_result(&Message::user("任务")));
        assert!(!has_tool_result(&Message::assistant("答")));
        // 图片后注（user + Image）不算工具结果
        let img = Message::User {
            content: vec![UserContent::image_base64("AAAA", None, None)],
        };
        assert!(!has_tool_result(&img));
    }
}
