//! ICA experimental evidence ledger for the Rust agent. Observations are records of
//! inputs/results, never automatic facts. Model tools cannot verify a claim.

use std::collections::HashSet;
use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::bridge::ToolSpec;

#[derive(Clone, Serialize, Deserialize)]
struct Event {
    id: String,
    source_type: String,
    origin_id: String,
    content: String,
    retracted: bool,
}

#[derive(Clone, Serialize, Deserialize)]
struct Claim {
    id: String,
    text: String,
    scope: String,
    state: String,
}

#[derive(Clone, Serialize, Deserialize)]
struct Evidence {
    id: String,
    claim_id: String,
    event_id: String,
    supports: bool,
    active: bool,
}

#[derive(Clone, Serialize, Deserialize)]
struct FutureItem {
    id: String,
    kind: String,
    text: String,
    predicted_observation: String,
    state: String,
}

#[derive(Clone, Serialize, Deserialize)]
struct Comparison {
    id: String,
    future_id: String,
    event_id: String,
    possible_match: bool,
    active: bool,
}

#[derive(Default, Serialize, Deserialize)]
pub struct IcaLedger {
    events: Vec<Event>,
    claims: Vec<Claim>,
    evidence: Vec<Evidence>,
    futures: Vec<FutureItem>,
    comparisons: Vec<Comparison>,
    dependencies: Vec<(String, String)>,
}

#[derive(Serialize, Deserialize)]
struct SavedLedger {
    version: u32,
    session_size: u64,
    ledger: IcaLedger,
}

fn required<'a>(args: &'a Value, key: &str) -> Result<&'a str, String> {
    args.get(key)
        .and_then(Value::as_str)
        .filter(|s| !s.trim().is_empty())
        .ok_or_else(|| format!("ICA 缺少字符串字段 {key}"))
}

fn limit_content(s: &str) -> String {
    s.chars().take(8192).collect()
}

impl IcaLedger {
    pub fn observe(&mut self, source_type: &str, origin_id: &str, content: &str) -> String {
        let id = format!("E{}", self.events.len() + 1);
        self.events.push(Event {
            id: id.clone(),
            source_type: source_type.into(),
            origin_id: origin_id.into(),
            content: limit_content(content),
            retracted: false,
        });
        id
    }

    fn event(&self, id: &str) -> Option<&Event> {
        self.events.iter().find(|e| e.id == id)
    }

    fn claim_exists(&self, id: &str) -> bool {
        self.claims.iter().any(|c| c.id == id)
    }

    fn reassess_claim(&mut self, id: &str) {
        let mut positive = HashSet::new();
        let mut negative = HashSet::new();
        for r in self
            .evidence
            .iter()
            .filter(|r| r.active && r.claim_id == id)
        {
            if let Some(e) = self.event(&r.event_id) {
                if e.retracted || e.source_type == "user" || e.source_type == "tool_error" {
                    continue;
                }
                if r.supports {
                    positive.insert(e.origin_id.clone());
                } else {
                    negative.insert(e.origin_id.clone());
                }
            }
        }
        if let Some(c) = self.claims.iter_mut().find(|c| c.id == id) {
            c.state = if !negative.is_empty() {
                "contested"
            } else if positive.len() >= 2 {
                "needs_review"
            } else if positive.len() == 1 {
                "partial"
            } else {
                "locked"
            }
            .into();
        }
    }

    fn reassess_future(&mut self, id: &str) {
        let found = self
            .comparisons
            .iter()
            .filter(|x| x.active && x.future_id == id);
        let (mut yes, mut no) = (false, false);
        for x in found {
            if x.possible_match {
                yes = true
            } else {
                no = true
            }
        }
        if let Some(f) = self.futures.iter_mut().find(|f| f.id == id) {
            f.state = match (yes, no) {
                (true, true) => "contested",
                (true, false) => "possible_match",
                (false, true) => "possible_mismatch",
                (false, false) => "proposed",
            }
            .into();
        }
    }

    fn mark_dependents_stale(&mut self, upstream: &str, seen: &mut HashSet<String>) {
        if !seen.insert(upstream.to_string()) {
            return;
        }
        let downstream: Vec<String> = self
            .dependencies
            .iter()
            .filter(|(u, _)| u == upstream)
            .map(|(_, d)| d.clone())
            .collect();
        for id in downstream {
            if let Some(c) = self.claims.iter_mut().find(|c| c.id == id) {
                c.state = "stale".into();
            }
            self.mark_dependents_stale(&id, seen);
        }
    }

    pub fn summary(&self) -> String {
        let mut s = format!(
            "ICA: {} observations, {} claims, {} future items.",
            self.events.len(),
            self.claims.len(),
            self.futures.len()
        );
        for c in self.claims.iter().rev().take(6).rev() {
            s.push_str(&format!(
                "\n{} [{}] {}",
                c.id,
                c.state,
                limit_content(&c.text)
            ));
        }
        for f in self.futures.iter().rev().take(4).rev() {
            s.push_str(&format!(
                "\n{} [{}/{}] {}",
                f.id,
                f.kind,
                f.state,
                limit_content(&f.text)
            ));
        }
        for e in self.events.iter().rev().take(6).rev() {
            s.push_str(&format!(
                "\n{} [{}/{}]{}",
                e.id,
                e.source_type,
                e.origin_id,
                if e.retracted { " retracted" } else { "" }
            ));
        }
        s
    }

    pub fn final_note(&self) -> String {
        if self.claims.is_empty() {
            "[ICA 证据账本原型] 本轮没有登记可核验主张；此功能不是原生 ICA 时间想象模型，也未验证这段回答。".into()
        } else {
            format!(
                "[ICA 证据账本原型] 主张状态仍需人工核验；{} 个主张，{} 个待审。此功能不是原生 ICA 时间想象模型。",
                self.claims.len(),
                self.claims
                    .iter()
                    .filter(|c| c.state == "needs_review")
                    .count()
            )
        }
    }

    pub fn execute(&mut self, name: &str, args: &Value) -> Result<String, String> {
        match name {
            "ica_propose_claim" => {
                let text = required(args, "text")?.to_string();
                let scope = args
                    .get("scope")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string();
                let id = format!("C{}", self.claims.len() + 1);
                self.claims.push(Claim {
                    id: id.clone(),
                    text,
                    scope,
                    state: "locked".into(),
                });
                Ok(format!("已登记 {id}，状态 locked；尚未核验。"))
            }
            "ica_propose_future" => {
                let kind = required(args, "kind")?;
                if !["hypothesis", "plan", "unknown_gap", "action_proposal"].contains(&kind) {
                    return Err("ICA future kind 无效".into());
                }
                let text = required(args, "text")?.to_string();
                let predicted_observation = args
                    .get("predicted_observation")
                    .and_then(Value::as_str)
                    .unwrap_or("")
                    .to_string();
                let id = format!("F{}", self.futures.len() + 1);
                self.futures.push(FutureItem {
                    id: id.clone(),
                    kind: kind.into(),
                    text,
                    predicted_observation,
                    state: "proposed".into(),
                });
                Ok(format!("已登记未来候选 {id}；没有执行动作。"))
            }
            "ica_cite_observation" => {
                let claim_id = required(args, "claim_id")?.to_string();
                let event_id = required(args, "event_id")?.to_string();
                if !self.claim_exists(&claim_id)
                    || !self.event(&event_id).is_some_and(|e| !e.retracted)
                {
                    return Err("ICA 引用的主张或观察不存在，或观察已撤回".into());
                }
                let supports = args
                    .get("supports")
                    .and_then(Value::as_bool)
                    .unwrap_or(true);
                let id = format!("R{}", self.evidence.len() + 1);
                self.evidence.push(Evidence {
                    id: id.clone(),
                    claim_id: claim_id.clone(),
                    event_id,
                    supports,
                    active: true,
                });
                self.reassess_claim(&claim_id);
                Ok(format!("已关联 {id}。{}", self.summary()))
            }
            "ica_compare_future" => {
                let future_id = required(args, "future_id")?.to_string();
                let event_id = required(args, "event_id")?.to_string();
                let possible_match = args
                    .get("possible_match")
                    .and_then(Value::as_bool)
                    .ok_or("ICA 缺少布尔字段 possible_match")?;
                if !self.futures.iter().any(|f| f.id == future_id)
                    || !self
                        .event(&event_id)
                        .is_some_and(|e| !e.retracted && e.source_type != "tool_error")
                {
                    return Err("ICA 对比的未来候选或观察不存在，或观察已失效".into());
                }
                let id = format!("X{}", self.comparisons.len() + 1);
                self.comparisons.push(Comparison {
                    id: id.clone(),
                    future_id: future_id.clone(),
                    event_id,
                    possible_match,
                    active: true,
                });
                self.reassess_future(&future_id);
                Ok(format!("已登记暂定比较 {id}。{}", self.summary()))
            }
            "ica_depend" => {
                let upstream = required(args, "upstream_id")?.to_string();
                let downstream = required(args, "downstream_id")?.to_string();
                if upstream == downstream
                    || !self.claim_exists(&upstream)
                    || !self.claim_exists(&downstream)
                {
                    return Err("ICA 依赖关系无效".into());
                }
                self.dependencies.push((upstream, downstream));
                Ok("ICA 依赖关系已登记。".into())
            }
            "ica_retract_observation" => {
                let id = required(args, "event_id")?.to_string();
                let Some(e) = self.events.iter_mut().find(|e| e.id == id && !e.retracted) else {
                    return Ok("观察不存在或已经撤回。".into());
                };
                e.retracted = true;
                let affected: HashSet<String> = self
                    .evidence
                    .iter_mut()
                    .filter(|r| r.event_id == id && r.active)
                    .map(|r| {
                        r.active = false;
                        r.claim_id.clone()
                    })
                    .collect();
                for claim in affected {
                    self.reassess_claim(&claim);
                    self.mark_dependents_stale(&claim, &mut HashSet::new());
                }
                let futures: HashSet<String> = self
                    .comparisons
                    .iter_mut()
                    .filter(|x| x.event_id == id && x.active)
                    .map(|x| {
                        x.active = false;
                        x.future_id.clone()
                    })
                    .collect();
                for future_id in futures {
                    self.reassess_future(&future_id);
                }
                Ok(format!("观察已撤回。{}", self.summary()))
            }
            "ica_status" => Ok(self.summary()),
            _ => Err(format!("未知 ICA 工具: {name}")),
        }
    }

    pub fn save(&self, session_file: &str) -> Result<(), String> {
        if session_file.is_empty() {
            return Ok(());
        }
        let session_size = fs::metadata(session_file).map_err(|e| e.to_string())?.len();
        let sidecar = format!("{session_file}.ica.json");
        let temp = format!("{sidecar}.{}.tmp", std::process::id());
        let bytes = serde_json::to_vec(&SavedLedger {
            version: 1,
            session_size,
            ledger: self.clone_for_save(),
        })
        .map_err(|e| e.to_string())?;
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&temp)
            .map_err(|e| e.to_string())?;
        file.write_all(&bytes).map_err(|e| e.to_string())?;
        file.sync_all().map_err(|e| e.to_string())?;
        fs::rename(&temp, &sidecar).map_err(|e| e.to_string())
    }

    fn clone_for_save(&self) -> Self {
        Self {
            events: self.events.clone(),
            claims: self.claims.clone(),
            evidence: self.evidence.clone(),
            futures: self.futures.clone(),
            comparisons: self.comparisons.clone(),
            dependencies: self.dependencies.clone(),
        }
    }

    pub fn load(session_file: &str) -> Result<Self, String> {
        let sidecar = format!("{session_file}.ica.json");
        let bytes = fs::read(&sidecar).map_err(|e| format!("ICA 账本读取失败 ({sidecar}): {e}"))?;
        let saved: SavedLedger =
            serde_json::from_slice(&bytes).map_err(|e| format!("ICA 账本解析失败: {e}"))?;
        let actual = fs::metadata(session_file).map_err(|e| e.to_string())?.len();
        if saved.version != 1 || saved.session_size != actual {
            return Err("ICA 账本与会话文件大小不一致，拒绝恢复".into());
        }
        Ok(saved.ledger)
    }
}

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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn copied_origin_and_user_statement_cannot_verify_claim() {
        let mut ledger = IcaLedger::default();
        let c = ledger
            .execute("ica_propose_claim", &json!({"text":"测试主张"}))
            .unwrap();
        assert!(c.contains("C1"));
        let user = ledger.observe("user", "user", "测试主张是真的");
        ledger
            .execute(
                "ica_cite_observation",
                &json!({"claim_id":"C1","event_id":user}),
            )
            .unwrap();
        assert_eq!(ledger.claims[0].state, "locked");
        for _ in 0..2 {
            let e = ledger.observe("tool", "same-tool", "重复来源");
            ledger
                .execute(
                    "ica_cite_observation",
                    &json!({"claim_id":"C1","event_id":e}),
                )
                .unwrap();
        }
        assert_eq!(ledger.claims[0].state, "partial");
        let e = ledger.observe("tool", "other-tool", "第二来源");
        ledger
            .execute(
                "ica_cite_observation",
                &json!({"claim_id":"C1","event_id":e}),
            )
            .unwrap();
        assert_eq!(ledger.claims[0].state, "needs_review");
    }

    #[test]
    fn retraction_stales_dependents_and_undoes_future_comparison() {
        let mut ledger = IcaLedger::default();
        ledger
            .execute("ica_propose_claim", &json!({"text":"上游"}))
            .unwrap();
        ledger
            .execute("ica_propose_claim", &json!({"text":"下游"}))
            .unwrap();
        ledger
            .execute(
                "ica_depend",
                &json!({"upstream_id":"C1","downstream_id":"C2"}),
            )
            .unwrap();
        ledger
            .execute(
                "ica_propose_future",
                &json!({"kind":"hypothesis","text":"未来"}),
            )
            .unwrap();
        let e = ledger.observe("tool", "sensor", "观察");
        ledger
            .execute(
                "ica_cite_observation",
                &json!({"claim_id":"C1","event_id":e}),
            )
            .unwrap();
        ledger
            .execute(
                "ica_compare_future",
                &json!({"future_id":"F1","event_id":e,"possible_match":true}),
            )
            .unwrap();
        ledger
            .execute("ica_retract_observation", &json!({"event_id":e}))
            .unwrap();
        assert_eq!(ledger.claims[0].state, "locked");
        assert_eq!(ledger.claims[1].state, "stale");
        assert_eq!(ledger.futures[0].state, "proposed");
    }

    #[test]
    fn sidecar_requires_the_matching_session_file() {
        let path = std::env::temp_dir().join(format!("bot4cj-ica-test-{}", std::process::id()));
        fs::write(&path, b"session").unwrap();
        let name = path.to_str().unwrap();
        let mut ledger = IcaLedger::default();
        ledger.observe("user", "user", "hello");
        ledger.save(name).unwrap();
        assert_eq!(IcaLedger::load(name).unwrap().events.len(), 1);
        fs::write(&path, b"session changed").unwrap();
        assert!(IcaLedger::load(name).is_err());
        fs::remove_file(&path).unwrap();
        fs::remove_file(format!("{name}.ica.json")).unwrap();
    }
}
