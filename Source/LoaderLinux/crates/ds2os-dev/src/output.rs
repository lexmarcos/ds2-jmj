//! One machine-readable result and an immutable directory per invocation.
use std::{io::{Read, Write}, path::{Path, PathBuf}, sync::Mutex, time::{Instant, SystemTime, UNIX_EPOCH}};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::os::unix::fs::MetadataExt;

struct Run { dir: PathBuf, json: bool, started: Instant, data: Value, outcome: Option<String>, events: std::fs::File, log_error: Option<String> }
static RUN: Mutex<Option<Run>> = Mutex::new(None);

pub fn id() -> String {
    format!("{}-{}", SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos(), std::process::id())
}
pub fn now_ms() -> u128 { SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() }

pub fn begin(machine: bool) -> Result<(), String> {
    let dir = crate::paths::state_dir().join("runs").join(id());
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let events = std::fs::File::create(dir.join("events.jsonl")).map_err(|e| e.to_string())?;
    std::fs::write(dir.join("command.json"), serde_json::to_vec_pretty(&json!({
        "schemaVersion": 1, "argv": std::env::args().collect::<Vec<_>>(), "atMs": now_ms()
    })).unwrap()).map_err(|e| e.to_string())?;
    *RUN.lock().unwrap() = Some(Run { dir, json: machine, started: Instant::now(), data: Value::Null, outcome: None, events, log_error: None });
    Ok(())
}

pub fn dir() -> PathBuf { RUN.lock().unwrap().as_ref().map(|r| r.dir.clone()).unwrap_or_else(crate::paths::log_dir) }
pub fn data(value: Value) { if let Some(run) = RUN.lock().unwrap().as_mut() { run.data = value; } }
pub fn outcome(value: &str) { if let Some(run) = RUN.lock().unwrap().as_mut() { run.outcome = Some(value.into()); } }
pub fn event(kind: &str, value: Value) {
    if let Some(run) = RUN.lock().unwrap().as_mut() {
        if let Err(e) = writeln!(run.events, "{}", json!({"atMs": now_ms(), "kind": kind, "data": value})) {
            run.log_error.get_or_insert_with(|| e.to_string());
        }
    }
}
pub fn line(args: std::fmt::Arguments<'_>) {
    let message = args.to_string();
    let machine = RUN.lock().unwrap().as_ref().is_some_and(|r| r.json);
    event("message", json!(message));
    if !machine { std::println!("{message}"); }
}

pub fn fingerprint(path: &Path) -> Value {
    let result = (|| -> std::io::Result<String> {
        let mut file = std::fs::File::open(path)?;
        let mut hash = Sha256::new();
        let mut buf = [0u8; 65536];
        loop { let n = file.read(&mut buf)?; if n == 0 { break; } hash.update(&buf[..n]); }
        Ok(format!("{:x}", hash.finalize()))
    })();
    match result { Ok(hash) => json!({"path": path, "sha256": hash}), Err(e) => json!({"path": path, "error": e.to_string()}) }
}

pub fn manifest(env: &crate::env::Environment) -> Result<(), String> {
    let installs: Vec<_> = env.installs.iter().map(|i| json!({
        "instance": i.account,
        "injector": fingerprint(&i.game_dir.join("Injector.dll")),
        "game": i.game_exe.as_ref().map(|p| fingerprint(p)),
        "configuredInjector": std::fs::read_to_string(i.game_dir.join("Injector.config")).ok(),
        "loadedHooks": crate::observe::hooks(&i.game_dir),
    })).collect();
    let manifest = json!({"schemaVersion": 1, "environment": env,
        "harness": fingerprint(&std::env::current_exe().map_err(|e| e.to_string())?),
        "server": env.server.as_ref().map(|s| fingerprint(&s.binary)), "installs": installs});
    std::fs::write(dir().join("manifest.json"), serde_json::to_vec_pretty(&manifest).unwrap()).map_err(|e| e.to_string())
}

/// Cheap metadata for every command; scenarios additionally hash all binaries.
pub fn environment(env: &crate::env::Environment) -> Result<(), String> {
    let instances: Vec<_> = env.installs.iter().map(|i| json!({"instance": i.account,
        "processes": crate::observe::processes(env, i.account),
        "configuredInjector": std::fs::read_to_string(i.game_dir.join("Injector.config")).ok(),
        "hooksReceipt": crate::observe::hooks(&i.game_dir)})).collect();
    std::fs::write(dir().join("environment.json"), serde_json::to_vec_pretty(&json!({
        "atMs": now_ms(), "environment": env, "instances": instances,
        "identities": crate::settings::HarnessConfig::load(),
    })).unwrap()).map_err(|e| e.to_string())
}

pub struct LogCursor { path: PathBuf, name: String, inode: u64, offset: u64 }
pub fn log_cursors(env: &crate::env::Environment) -> Vec<LogCursor> {
    let mut files = vec![(crate::paths::server_log(), "server.log".into())];
    for i in &env.installs {
        files.push((crate::paths::instance_log(i.account), format!("instance-{}.log", i.account)));
        for name in ["DS2OS_Injector.log", "DS2_Seamless.log", "DS2_Session.log", "DS2_Respawn.log"] {
            files.push((i.game_dir.join(name), format!("instance-{}-{name}", i.account)));
        }
    }
    files.into_iter().map(|(path, name)| {
        let meta = std::fs::metadata(&path).ok();
        LogCursor { path, name, inode: meta.as_ref().map(|m| m.ino()).unwrap_or(0), offset: meta.map(|m| m.len()).unwrap_or(0) }
    }).collect()
}
pub fn capture_logs(cursors: &[LogCursor]) -> Result<(), String> {
    use std::io::{Seek, SeekFrom};
    let dest = dir().join("logs");
    std::fs::create_dir_all(&dest).map_err(|e| e.to_string())?;
    let mut index = Vec::new();
    for cursor in cursors {
        let result = (|| -> std::io::Result<Value> {
            let mut file = std::fs::File::open(&cursor.path)?;
            let meta = file.metadata()?;
            let rotated = meta.ino() != cursor.inode || meta.len() < cursor.offset;
            let start = if rotated { 0 } else { cursor.offset };
            let kept_start = start.max(meta.len().saturating_sub(1024 * 1024));
            file.seek(SeekFrom::Start(kept_start))?;
            let mut out = std::fs::File::create(dest.join(&cursor.name))?;
            std::io::copy(&mut file.take(meta.len().saturating_sub(kept_start)), &mut out)?;
            Ok(json!({"source":cursor.path,"artifact":cursor.name,"fromByte":kept_start,"toByte":meta.len(),
                "rotated":rotated,"truncated":kept_start > start}))
        })();
        index.push(result.unwrap_or_else(|e| json!({"source":cursor.path,"error":e.to_string()})));
    }
    std::fs::write(dest.join("index.json"), serde_json::to_vec_pretty(&index).unwrap()).map_err(|e| e.to_string())
}

pub fn finish(result: Result<(), String>) -> i32 {
    let mut guard = RUN.lock().unwrap();
    let Some(mut run) = guard.take() else { return 1; };
    let result = if let Some(e) = &run.log_error {
        run.outcome = Some("failed".into());
        Err(format!("artifact_error: {e}; execution_error: {:?}", result.err()))
    } else { result };
    let status = run.outcome.unwrap_or_else(|| if result.is_ok() { "passed" } else { "failed" }.into());
    let mut code = if result.is_ok() && status == "passed" { 0 } else if status == "inconclusive" { 2 } else { 1 };
    let error = result.err();
    let error_code = error.as_deref().map(|e| e.split_once(':').map(|(code, _)| code)
        .filter(|c| !c.is_empty() && c.bytes().all(|b| b.is_ascii_lowercase() || b == b'_'))
        .unwrap_or("operation_failed"));
    let mut payload = json!({"schemaVersion": 1, "runId": run.dir.file_name().unwrap().to_string_lossy(),
        "status": status, "ok": code == 0, "durationMs": run.started.elapsed().as_millis(),
        "data": run.data, "error": error, "errorCode": error_code, "artifacts": run.dir });
    let saved = (|| -> std::io::Result<()> {
        writeln!(run.events, "{}", json!({"atMs": now_ms(), "kind": "result", "data": payload}))?;
        run.events.sync_all()?;
        std::fs::write(run.dir.join("result.json"), serde_json::to_vec_pretty(&payload).unwrap())
    })();
    if let Err(e) = saved { code = 1; payload["ok"] = json!(false); payload["status"] = json!("failed"); payload["artifactError"] = json!(e.to_string()); }
    if run.json { std::println!("{}", serde_json::to_string_pretty(&payload).unwrap()); }
    else { if !payload["error"].is_null() { eprintln!("erro: {}", payload["error"].as_str().unwrap_or_default()); }
        eprintln!("{} — evidências: {}", payload["status"].as_str().unwrap_or_default(), run.dir.display()); }
    code
}
