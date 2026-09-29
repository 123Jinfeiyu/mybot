//! ICA 账本的 sidecar 持久化（2026-09-29 B1 拆自 ica.rs）。
//!
//! 契约：`<会话文件>.ica.json`，权限 0600，临时文件 + fsync + rename 原子写；
//! 恢复要求 sidecar 记录的会话文件长度与当前文件一致（会话变了账本就不该
//! 被复用）。语义与拆分前逐字保持。

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::os::unix::fs::OpenOptionsExt;

use serde::{Deserialize, Serialize};

use super::IcaLedger;

#[derive(Serialize, Deserialize)]
struct SavedLedger {
    version: u32,
    session_size: u64,
    ledger: IcaLedger,
}

impl IcaLedger {
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
            current_task: self.current_task,
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
