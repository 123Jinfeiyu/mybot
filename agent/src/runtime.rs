//! Agent 运行时装配：审计日志与会话文件写入器。

use crate::audit::AuditLog;
use crate::session::{self, SessionFileWriter, SessionHead};
use crate::session_store::{SessionMode, SessionPlan};

pub fn open_audit(path: &str, resumed_session_id: &str) -> Result<Option<AuditLog>, String> {
    if path.is_empty() {
        return Ok(None);
    }
    let mut audit = AuditLog::open(path).map_err(|e| e.to_string())?;
    audit.adopt_session_id(resumed_session_id);
    Ok(Some(audit))
}

pub fn open_session_writer(
    plan: Option<&SessionPlan>,
    audit: Option<&AuditLog>,
    material: &crate::prompt::PromptMaterial,
    tool_names: &[String],
    preamble: &str,
) -> Result<Option<SessionFileWriter>, String> {
    let Some(plan) = plan else {
        return Ok(None);
    };

    let mut writer = SessionFileWriter::new();
    writer.open(&plan.path, plan.append)?;
    if plan.mode == SessionMode::New {
        let session_id = audit
            .map(|a| a.session_id().to_string())
            .unwrap_or_default();
        writer.write_header(&SessionHead {
            version: session::SESSION_FILE_VERSION,
            session_id,
            robot: material.robot_name.clone(),
            tools_fp: session::tools_fingerprint(tool_names),
            system_fp: crate::audit::fingerprint64(preamble),
        });
    }
    Ok(Some(writer))
}
