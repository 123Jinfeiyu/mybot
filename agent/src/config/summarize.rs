//! 配置摘要渲染（`--check-config` 输出；拆自 config/mod.rs，2026-10-01）。
//!
//! **只渲染、不判定**（校验单点在 mod.rs 的 build），密钥**只报来源不报值**
//! （与 C++ `summarizeAgentConfig` 同契约）。

use super::{Config, SessionPathSource};

/// 配置摘要（`--check-config`）：**只渲染、不判定**（校验单点在 [`build`]），
/// 密钥**只报来源不报值**（与 C++ `summarizeAgentConfig` 同契约）。
pub fn summarize(cfg: &Config) -> String {
    let or_none = |s: &str, fallback: &str| {
        if s.is_empty() {
            fallback.to_string()
        } else {
            s.to_string()
        }
    };
    let key_source = match cfg.api_key_source {
        "env" => "环境变量",
        "config_inline" => "配置文件 api_key 字段",
        "config_file" => "配置文件 api_key_file",
        _ => "未解析到",
    };
    let mut s = String::new();
    s += "配置校验通过：\n";
    s += &format!(
        "  配置文件    : {}\n",
        or_none(&cfg.config_path, "(未使用)")
    );
    s += &format!("  后端 socket : {}\n", cfg.socket);
    s += &format!("  协议驱动    : {}（变量 {}）\n", cfg.driver, cfg.llm_var);
    s += &format!("  模型        : {}\n", cfg.model);
    s += &format!("  接入点      : {}\n", cfg.endpoint);
    s += &format!(
        "  采样        : max_tokens={} temperature={} top_p={} timeout={}s\n",
        cfg.max_tokens, cfg.temperature, cfg.top_p, cfg.timeout_sec
    );
    s += &format!(
        "  轮次预算    : max_turns={}（rig 的模型调用预算，含重试）\n",
        cfg.max_turns
    );
    s += &format!(
        "  输出模式    : {}\n",
        if cfg.stream {
            "流式（逐 delta 打到 stdout）"
        } else {
            "阻塞（整段输出；--llm-no-stream 或配置文件 stream=false）"
        }
    );
    s += &format!("  API key     : {}（值不显示）\n", key_source);
    s += &format!(
        "  提示词包    : {} 项，{} 字节\n",
        cfg.prompt_files.len(),
        cfg.prompt_pack.len()
    );
    s += &format!(
        "  墙钟预算    : {}（单任务）\n",
        if cfg.max_task_seconds == 0 {
            "不限".to_string()
        } else {
            format!("{} 秒", cfg.max_task_seconds)
        }
    );
    // 字节预算与重试（批次 C 起生效；只渲染、不判定）
    let bytes = |n: i64| {
        if n == 0 {
            "不限".to_string()
        } else {
            format!("{n} 字节")
        }
    };
    s += &format!(
        "  历史/任务预算: 历史 {} / 任务文本 {} / 任务图片 {}（单轮调用上限 {} 条）\n",
        bytes(cfg.max_history_bytes),
        bytes(cfg.max_task_tool_result_bytes),
        bytes(cfg.max_task_image_bytes),
        if cfg.max_calls_per_round == 0 {
            "不限".to_string()
        } else {
            cfg.max_calls_per_round.to_string()
        }
    );
    s += &format!(
        "  历史摘要    : {}\n",
        if cfg.max_history_bytes == 0 {
            "不适用（历史不裁剪）".to_string()
        } else if cfg.history_summary {
            format!(
                "开（被裁任务块压成摘要，正文上限 {} 字节）",
                cfg.max_summary_bytes
            )
        } else {
            "关（history_summary=false：被裁任务块直接丢弃）".to_string()
        }
    );
    s += &format!(
        "  网络重试    : 尝试 {} 次（含首次），退避基础 {}ms（指数增长，单次钳到 60s）\n",
        cfg.max_retries, cfg.retry_backoff_ms
    );
    s += &format!("  审计轨迹    : {}\n", or_none(&cfg.audit_log, "(未开启)"));
    // 状态目录与落点：`--check-config` 在连后端之前返回，此刻还拿不到机器人名，
    // 故只报**根与模式**，不臆造具体落点（具体落点由启动时的 stderr 行给出）。
    let dir_source = match cfg.state_dir_source {
        "env" => "环境变量 MYBOT_HOME",
        "home" => "家目录",
        _ => "不可用",
    };
    s += &format!(
        "  状态目录    : {}{}\n",
        or_none(&cfg.state_dir, "(不可用)"),
        if cfg.state_dir_ok {
            format!("（来源 {dir_source}）")
        } else {
            String::new()
        }
    );
    s += &format!(
        "  会话持久化  : {}\n",
        match cfg.session_path_source {
            SessionPathSource::Cli if cfg.resume => format!("{}（--resume）", cfg.session_file),
            SessionPathSource::Cli => cfg.session_file.clone(),
            SessionPathSource::Config =>
                format!("{}（配置文件来源，只写不可读）", cfg.session_file),
            SessionPathSource::Default if !cfg.state_dir_ok =>
                "(未开启：无家目录，可用 MYBOT_HOME 指定)".to_string(),
            SessionPathSource::Default if cfg.new_session =>
                format!("{}（--new-session：强制新开一段）", cfg.state_dir),
            SessionPathSource::Default => format!("{}（缺省按机器人续接上一次）", cfg.state_dir),
        }
    );
    if !cfg.config_warning.is_empty() {
        s += &format!("  配置告警    : {}\n", cfg.config_warning);
    }
    s
}
