//! Exercise the real binary's output contract without starting games or touching live harness state.
use std::path::PathBuf;
use std::process::{Command, Output};
use std::time::{SystemTime, UNIX_EPOCH};
use serde_json::{json, Value};

struct Sandbox(PathBuf);
impl Sandbox {
    fn new() -> Self {
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let path = std::env::temp_dir().join(format!("ds2-cli-{}-{nonce}", std::process::id()));
        std::fs::create_dir_all(&path).unwrap();
        Self(path)
    }

    fn run(&self, args: &[&str]) -> (Output, Value) {
        let output = Command::new(env!("CARGO_BIN_EXE_ds2os-dev"))
            .env("XDG_DATA_HOME", &self.0)
            .args(args)
            .output()
            .unwrap();
        let value: Value = serde_json::from_slice(&output.stdout)
            .unwrap_or_else(|e| panic!("invalid JSON: {e}; stderr={}", String::from_utf8_lossy(&output.stderr)));
        assert_eq!(value["schemaVersion"], 1);
        assert!(value["artifacts"].as_str().unwrap().starts_with(self.0.to_str().unwrap()));
        let saved: Value = serde_json::from_slice(&std::fs::read(
            PathBuf::from(value["artifacts"].as_str().unwrap()).join("result.json")
        ).unwrap()).unwrap();
        assert_eq!(saved, value);
        (output, value)
    }
}
impl Drop for Sandbox {
    fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.0); }
}

#[test]
fn doctor_json_and_exit_code_agree_even_when_environment_is_incomplete() {
    let sandbox = Sandbox::new();
    let (result, value) = sandbox.run(&["doctor", "--json"]);
    let ready = value["data"]["ok"].as_bool().unwrap();
    assert_eq!(result.status.code(), Some(if ready { 0 } else { 1 }));
    assert_eq!(value["ok"], ready);
    // Only a problem fails the command; a check that could not run is never ok.
    let checks = value["data"]["checks"].as_array().unwrap();
    assert!(checks.iter().all(|c| ["ok", "warning", "problem", "skipped"].contains(&c["status"].as_str().unwrap())));
    assert_eq!(ready, checks.iter().all(|c| c["status"] != "problem"));
    let commit = value["harnessBuild"]["commit"].as_str().unwrap();
    assert!(commit == "unknown" || (commit.len() == 40 && commit.bytes().all(|b| b.is_ascii_hexdigit())), "{commit}");
    assert!(value["harnessBuild"]["dirty"].is_boolean());
}

#[test]
fn scenario_validation_never_executes_its_steps() {
    let sandbox = Sandbox::new();
    let (result, value) = sandbox.run(&["--json", "scenario", "validate", "world-ready"]);
    assert!(result.status.success());
    assert_eq!(value["data"]["valid"], true);
    let file = sandbox.0.join("bad.json");
    std::fs::write(&file, json!({"schemaVersion":1,"name":"bad","instances":[1],
        "steps":[{"action":"input","instance":1,"command":"press a"}]}).to_string()).unwrap();
    let (result, value) = sandbox.run(&["scenario", "validate", file.to_str().unwrap(), "--json"]);
    assert_eq!(result.status.code(), Some(1));
    assert_eq!(value["errorCode"], "invalid_scenario");
}

#[test]
fn control_lock_returns_busy_without_touching_the_pad() {
    let sandbox = Sandbox::new();
    let dir = sandbox.0.join("ds2os-dev");
    std::fs::create_dir_all(&dir).unwrap();
    let lock = std::fs::File::create(dir.join("control.lock")).unwrap();
    lock.try_lock().unwrap();
    let (result, value) = sandbox.run(&["pad", "press", "a", "--json"]);
    assert_eq!(result.status.code(), Some(1));
    assert_eq!(value["errorCode"], "busy");
}

#[test]
fn each_invocation_preserves_a_distinct_result() {
    let sandbox = Sandbox::new();
    let (_, first) = sandbox.run(&["save", "list", "--json"]);
    let (_, second) = sandbox.run(&["save", "list", "--json"]);
    assert_ne!(first["runId"], second["runId"]);
    assert!(first["data"]["snapshots"].is_array());
    assert!(PathBuf::from(first["artifacts"].as_str().unwrap()).join("result.json").is_file());
}
