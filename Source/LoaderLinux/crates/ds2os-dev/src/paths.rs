//! Where everything the harness owns lives.
//!
//! One predictable directory, stable file names. When something goes wrong the
//! first question is always "what was run and what did it print", and both
//! answers are in here.

use std::path::PathBuf;

/// Root of the harness state: logs, pidfiles, the resolved environment.
pub fn state_dir() -> PathBuf {
    let base = std::env::var("XDG_DATA_HOME")
        .ok()
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| home().join(".local/share"));
    base.join("ds2os-dev")
}

/// Settings the harness remembers between runs.
pub fn harness_config() -> PathBuf {
    state_dir().join("config.json")
}

pub fn log_dir() -> PathBuf {
    state_dir().join("logs")
}

pub fn home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").unwrap_or_default())
}

/// Log of every command the harness ran, with timestamps.
pub fn cli_log() -> PathBuf {
    log_dir().join("cli.log")
}

pub fn server_log() -> PathBuf {
    log_dir().join("server.log")
}

pub fn instance_log(instance: u8) -> PathBuf {
    log_dir().join(format!("instance-{instance}.log"))
}

pub fn server_pid() -> PathBuf {
    state_dir().join("server.pid")
}

pub fn instance_pid(instance: u8) -> PathBuf {
    state_dir().join(format!("instance-{instance}.pid"))
}

/// The Proton prefix the second instance runs in. It has to be separate from
/// the one Steam uses: named kernel objects are per prefix, so the game's
/// single-instance mutex in Steam's prefix is invisible here.
pub fn second_prefix() -> PathBuf {
    state_dir().join("second-instance")
}

pub fn ensure_dirs() -> std::io::Result<()> {
    std::fs::create_dir_all(log_dir())
}
