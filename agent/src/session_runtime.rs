//! Agent 会话运行时装配：provider 与 SessionAssembly。

use std::sync::Arc;

use crate::bridge::Bridge;
use crate::config::Config;
use crate::hook::HookState;
use crate::prompt::PromptMaterial;
use crate::provider;
use crate::repl::{ResumeContext, SessionAssembly};
use crate::session::SessionFileWriter;
use crate::toolspec::ToolSpec;

pub struct SessionParts {
    pub assembly: SessionAssembly,
}

pub struct SessionRuntimeInput {
    pub cfg: Config,
    pub specs: Vec<ToolSpec>,
    pub bridge: Arc<Bridge>,
    pub state: Arc<HookState>,
    pub preamble: String,
    pub tool_names: Vec<String>,
    pub material: PromptMaterial,
    pub audit: Option<crate::audit::AuditLog>,
    pub writer: Option<SessionFileWriter>,
    pub history: Vec<rig_agent::completion::Message>,
    pub summary: Option<String>,
    pub resumed: bool,
}

pub fn build_session_parts(input: SessionRuntimeInput) -> Result<SessionParts, anyhow::Error> {
    let backend = provider::build(
        &input.cfg,
        input.specs,
        &input.bridge,
        Arc::clone(&input.state),
        &input.preamble,
    )?;
    let assembly = SessionAssembly {
        cfg: input.cfg,
        backend,
        bridge: input.bridge,
        state: input.state,
        tool_names: input.tool_names,
        material: input.material,
        audit: input.audit,
        writer: input.writer,
        resume: ResumeContext {
            history: input.history,
            summary: input.summary,
            resumed: input.resumed,
        },
    };
    Ok(SessionParts { assembly })
}
