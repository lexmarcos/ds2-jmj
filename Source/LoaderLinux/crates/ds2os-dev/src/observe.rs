//! Correlated observations. Server presence does not establish a working P2P session.
use std::{path::Path, time::{Duration, Instant}};
use serde::Serialize;
use serde_json::{json, Value};
use crate::{api, env::{Environment, Install}, game, nav, probe::{self, Reason, Where}, settings::HarnessConfig};

#[derive(Debug, Clone, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct Process { pub pid: u32, pub start_ticks: u64 }

pub fn processes(env: &Environment, account: u8) -> Vec<Process> {
    let Ok(prefix) = game::compat_data(env, account) else { return vec![]; };
    let mut found: Vec<_> = game::instance_pids(&prefix).into_iter().filter_map(|pid| {
        let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
        let (_, fields) = stat.rsplit_once(')')?;
        Some(Process { pid, start_ticks: fields.split_whitespace().nth(19)?.parse().ok()? })
    }).collect();
    found.sort_by_key(|p| p.pid);
    found
}

pub fn steam_id(account: u8) -> Result<String, String> {
    HarnessConfig::load().steam_ids.get(&account).cloned().ok_or_else(||
        format!("identity_unresolved: configure `ds2os-dev game identity --instance {account} <steam-id-64>`"))
}

pub fn player_for<'a>(players: &'a [api::Player], id: &str) -> Result<Option<&'a api::Player>, String> {
    let found: Vec<_> = players.iter().filter(|p| p.steam_id == id).collect();
    if found.len() > 1 { return Err("ambiguous_identity: mais de uma conexão para a conta".into()); }
    Ok(found.first().copied())
}

/// Boot ID is optional for legacy DLLs. Tick progression and process identity
/// still gate their positions; hook assertions require the new receipt.
pub fn sample(dir: &Path) -> Option<(u64, Option<String>)> {
    let path = dir.join("DS2_Nav.txt");
    if age_ms(&path)? > 2000 { return None; }
    let raw = std::fs::read_to_string(path).ok()?;
    let fields: Vec<_> = raw.split_whitespace().collect();
    let (tick, boot) = if fields.first() == Some(&"sem") { (2, 3) } else { (6, 8) };
    Some((fields.get(tick)?.parse().ok()?, fields.get(boot).map(|s| s.to_string())))
}
pub fn age_ms(path: &Path) -> Option<u128> {
    Some(std::fs::metadata(path).ok()?.modified().ok()?.elapsed().ok()?.as_millis())
}

/// What an install's executable is, as far as the version-specific offsets are
/// concerned.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Build {
    /// The build every offset was measured against.
    Expected,
    /// Another build: nothing version specific may be asked of it.
    Other,
    /// No executable resolved, or it cannot be read.
    Missing,
}

/// The executable checked is the one the environment resolved for the install
/// (`Game/DarkSoulsII.exe` for Scholar of the First Sin), not a file in the
/// install root: looking there found nothing, so from 13/09 every probe
/// answered `unknown` and `game enter` never pressed a button.
pub fn build_of(install: &Install, expected: ds2os_core::exe::Fingerprint) -> Build {
    match install.game_exe.as_deref().map(fingerprint) {
        Some(Some(taken)) if taken == expected => Build::Expected,
        Some(Some(_)) => Build::Other,
        _ => Build::Missing,
    }
}

/// Hashing 28 MB on every probe would be slow, so the fingerprint is kept
/// until the file's size or modification time changes.
fn fingerprint(exe: &Path) -> Option<ds2os_core::exe::Fingerprint> {
    use std::sync::{Mutex, OnceLock};
    use std::collections::HashMap;
    use std::path::PathBuf;
    use std::time::SystemTime;
    use ds2os_core::exe::Fingerprint;
    type Cache = HashMap<PathBuf, (SystemTime, u64, Option<Fingerprint>)>;
    static CACHE: OnceLock<Mutex<Cache>> = OnceLock::new();
    let meta = std::fs::metadata(exe).ok()?;
    let modified = meta.modified().ok()?;
    let mut cache = CACHE.get_or_init(|| Mutex::new(HashMap::new())).lock().unwrap();
    if let Some(&(old, size, taken)) = cache.get(exe) {
        if old == modified && size == meta.len() { return taken; }
    }
    let taken = ds2os_core::exe::fingerprint(exe).ok();
    cache.insert(exe.to_path_buf(), (modified, meta.len(), taken));
    taken
}
pub fn hooks(dir: &Path) -> Option<Value> {
    let (_, boot) = sample(dir)?;
    let boot = boot?;
    let value: Value = serde_json::from_slice(&std::fs::read(dir.join("DS2_Harness.json")).ok()?).ok()?;
    (value.get("schemaVersion")?.as_u64()? == 1 && value.get("bootId")?.as_str()? == boot).then_some(value)
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Instance {
    pub instance: u8,
    pub observed_at_ms: u128,
    pub processes: Vec<Process>,
    pub steam_id: Option<String>,
    pub identity_source: &'static str,
    pub state: Where,
    /// Why `state` is what it is; see `probe::Reason`.
    pub state_reason: Reason,
    pub pose: Option<nav::Pose>,
    pub pose_age_ms: Option<u128>,
    pub boot_id: Option<String>,
    pub player: Option<api::Player>,
    pub server_connected: Option<bool>,
    pub p2p_session_verified: Option<bool>,
    pub hooks: Option<Value>,
    /// Only when asked for: it costs another MemProbe round trip.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub character: Option<crate::memory::Character>,
    pub problems: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Observation {
    pub schema_version: u8,
    pub observed_at_ms: u128,
    pub duration_ms: u128,
    pub server_error: Option<String>,
    pub server_observed_at_ms: u128,
    pub instances: Vec<Instance>,
}

pub fn collect(env: &Environment, accounts: &[u8]) -> Observation {
    collect_until(env, accounts, crate::control::Deadline::after(Duration::from_secs(20)))
}

pub fn collect_until(env: &Environment, accounts: &[u8], deadline: crate::control::Deadline) -> Observation {
    collect_with(env, accounts, deadline, false)
}

/// `character` adds the local character read from memory, for instances
/// confirmed in the world.
pub fn collect_with(env: &Environment, accounts: &[u8], deadline: crate::control::Deadline, character: bool) -> Observation {
    let started = Instant::now();
    let server_players = env.server.as_ref().ok_or_else(|| "servidor não encontrado".to_string())
        .and_then(|s| api::players_until(s, api::web_port(&s.config), deadline.remaining()?.min(Duration::from_secs(10))));
    let server_observed_at_ms = crate::output::now_ms();
    let mut instances = Vec::new();
    for &account in accounts {
        let pids = processes(env, account);
        let id = steam_id(account);
        let mut item = Instance { instance: account, observed_at_ms: crate::output::now_ms(), processes: pids.clone(),
            steam_id: id.clone().ok(), identity_source: "harness_config", state: Where::Unknown,
            state_reason: Reason::InstanceStopped, pose: None, pose_age_ms: None, boot_id: None, player: None,
            server_connected: None, p2p_session_verified: None, hooks: None, character: None, problems: Vec::new() };
        if let Err(e) = &id { item.problems.push(e.clone()); }
        if let (Ok(id), Ok(players)) = (&id, &server_players) {
            match player_for(players, id) {
                Ok(player) => { item.player = player.cloned(); item.server_connected = Some(player.is_some()); }
                Err(e) => item.problems.push(e),
            }
        }
        match env.installs.iter().find(|i| i.account == account) {
            None => {
                item.state_reason = Reason::InstanceMissing;
                item.problems.push("instance_missing: instalação não encontrada".into());
            }
            Some(_) if pids.is_empty() => item.problems.push("instance_stopped: processo da instância ausente".into()),
            Some(install) => match deadline.remaining() {
                Err(e) => {
                    item.state_reason = if e.starts_with("cancelled") { Reason::Cancelled } else { Reason::Timeout };
                    item.problems.push(format!("state_unknown: {e}"));
                }
                Ok(left) => {
                    let before = sample(&install.game_dir);
                    let located = probe::locate(install, left.min(Duration::from_secs(2)));
                    (item.state, item.state_reason) = (located.state, located.reason);
                    if located.state == Where::Unknown { item.problems.push(format!("state_unknown: {}", located.reason)); }
                    let _ = deadline.sleep(Duration::from_millis(100));
                    let after = sample(&install.game_dir);
                    let fresh = match (&before, &after) {
                        (Some((a, boot_a)), Some((b, boot_b))) => b > a && boot_a == boot_b,
                        _ => false,
                    };
                    if fresh && processes(env, account) == pids {
                        item.boot_id = after.and_then(|(_, b)| b);
                        item.hooks = hooks(&install.game_dir);
                        if item.state == Where::World { item.pose = nav::read(&install.game_dir); }
                        item.pose_age_ms = age_ms(&install.game_dir.join("DS2_Nav.txt"));
                        if character && item.state == Where::World {
                            match deadline.remaining().and_then(|left| crate::memory::read(install, left.min(Duration::from_secs(3)))) {
                                Ok(read) => item.character = Some(read),
                                Err(e) => item.problems.push(format!("character_unread: {e}")),
                            }
                        }
                    } else {
                        // An unknown keeps the probe's own reason; a known state
                        // without fresh telemetry is not believed.
                        if item.state != Where::Unknown { (item.state, item.state_reason) = (Where::Unknown, Reason::StaleTelemetry); }
                        item.problems.push("stale_telemetry: sem amostra nova do mesmo processo/boot".into());
                    }
                }
            },
        }
        item.observed_at_ms = crate::output::now_ms();
        instances.push(item);
    }
    Observation { schema_version: 1, observed_at_ms: crate::output::now_ms(), duration_ms: started.elapsed().as_millis(),
        server_error: server_players.err(), server_observed_at_ms, instances }
}

pub fn command(env: &Environment, accounts: &[u8], character: bool) -> Result<(), String> {
    let observation = collect_with(env, accounts, crate::control::Deadline::after(Duration::from_secs(20)), character);
    crate::output::data(json!(observation));
    for i in &observation.instances { println!("conta {}: {} ({}), personagem {}, posição {:?}", i.instance, i.state, i.state_reason,
        i.player.as_ref().map(|p| p.name.as_str()).unwrap_or("desconhecido"), i.pose); }
    if observation.server_error.is_some() || observation.instances.iter().any(|i| !i.problems.is_empty() || i.state == Where::Unknown) {
        crate::output::outcome("inconclusive");
        return Err("observation_incomplete: veja data.instances[].problems e serverError".into());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn player(id: &str) -> api::Player {
        api::Player { steam_id: id.into(), name: "same-name".into(), player_id: 1, soul_level: 1, souls: None,
            soul_memory: 1, death_count: None, multiplay_count: None, covenant: String::new(), status: String::new(), location: String::new(), play_time: String::new() }
    }
    #[test]
    fn the_build_is_read_from_the_executable_the_install_resolved() {
        // Scholar of the First Sin's layout: the executable under Game/, the
        // injector and its request files at the root.
        let root = std::env::temp_dir().join(format!("ds2os-install-{}", crate::output::id()));
        std::fs::create_dir_all(root.join("Game")).unwrap();
        let exe = root.join("Game").join("DarkSoulsII.exe");
        std::fs::write(&exe, b"hello").unwrap();
        let hello = ds2os_core::exe::fingerprint(&exe).unwrap();
        let install = Install { account: 1, steam_root: root.clone(), game_dir: root.clone(), game_exe: Some(exe), prefix: None };

        assert_eq!(build_of(&install, hello), Build::Expected);
        assert_eq!(build_of(&Install { game_exe: Some(root.join("DarkSoulsII.exe")), ..install.clone() }, hello), Build::Missing);
        assert_eq!(build_of(&Install { game_exe: None, ..install.clone() }, hello), Build::Missing);
        // Any other build is not the one the offsets were measured against.
        assert_eq!(build_of(&install, ds2os_core::exe::DS2_SOTFS_1_03), Build::Other);
        std::fs::remove_dir_all(&root).ok();
    }

    #[test]
    fn another_instance_with_the_same_name_is_not_ours() {
        let list = vec![player("111"), player("222")];
        assert_eq!(player_for(&list, "111").unwrap().unwrap().steam_id, "111");
        assert!(player_for(&list, "333").unwrap().is_none());
        assert!(player_for(&[player("111"), player("111")], "111").is_err());
    }
}
