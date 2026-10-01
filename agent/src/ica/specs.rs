//! 八个 ICA 本地工具的 schema 声明（2026-09-29 B1 拆自 ica.rs，逐字保持）。

use serde_json::{json, Value};

use crate::toolspec::ToolSpec;

pub fn specs() -> Vec<ToolSpec> {
    fn spec(name: &str, description: &str, schema: Value) -> ToolSpec {
        ToolSpec {
            name: name.into(),
            description: description.into(),
            parameters: schema,
        }
    }
    vec![
        spec(
            "ica_propose_claim",
            "记录待核验事实主张；不会自动验证。",
            json!({"type":"object","properties":{"text":{"type":"string"},"scope":{"type":"string"}},"required":["text"]}),
        ),
        spec(
            "ica_propose_future",
            "记录假设、计划、未知缺口或行动建议；不执行。",
            json!({"type":"object","properties":{"kind":{"type":"string","enum":["hypothesis","plan","unknown_gap","action_proposal"]},"text":{"type":"string"},"predicted_observation":{"type":"string"}},"required":["kind","text"]}),
        ),
        spec(
            "ica_propose_action",
            "在执行现实动作前，登记工具、完整参数、理由和预期可观察结果；登记不执行动作。",
            json!({"type":"object","properties":{"tool":{"type":"string","enum":["run_action_script","apply_sql","run_command"]},"arguments":{"type":"object"},"reason":{"type":"string"},"predicted_observation":{"type":"string"}},"required":["tool","arguments","reason","predicted_observation"]}),
        ),
        spec(
            "ica_compare_future",
            "把未来候选和之后的观察作暂定比较，不是事实验证。",
            json!({"type":"object","properties":{"future_id":{"type":"string"},"event_id":{"type":"string"},"possible_match":{"type":"boolean"}},"required":["future_id","event_id","possible_match"]}),
        ),
        spec(
            "ica_cite_observation",
            "把已有观察关联到主张；同来源只计一次。",
            json!({"type":"object","properties":{"claim_id":{"type":"string"},"event_id":{"type":"string"},"supports":{"type":"boolean"}},"required":["claim_id","event_id"]}),
        ),
        spec(
            "ica_depend",
            "记录主张间的依赖。",
            json!({"type":"object","properties":{"upstream_id":{"type":"string"},"downstream_id":{"type":"string"}},"required":["upstream_id","downstream_id"]}),
        ),
        spec(
            "ica_retract_observation",
            "撤回观察并使相关证据失效。",
            json!({"type":"object","properties":{"event_id":{"type":"string"}},"required":["event_id"]}),
        ),
        spec(
            "ica_status",
            "读取观察、主张、未来候选及其状态。",
            json!({"type":"object","properties":{}}),
        ),
    ]
}
