//! The local DS3OS server: its configuration, its lifetime and its log.

use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::{json, Value};

use crate::env::{Environment, ServerPaths};
use crate::paths;
use crate::proc;

/// Ports the server listens on, in the order the game uses them.
pub const PORTS: [(&str, u16); 4] =
    [("login", 50050), ("auth", 50000), ("game", 50010), ("webui", 50005)];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerStatus {
    pub running: bool,
    pub pid: Option<u32>,
    pub ports_listening: Vec<String>,
    pub players: Option<u32>,
    pub config_ok: bool,
    pub log: String,
}

/// The settings a local PvP test needs, whatever else is in the file.
///
/// GameType is the one that silently ruins everything: the default is
/// DarkSouls3, and a server speaking the wrong protocol never answers the game.
fn required_settings() -> Value {
    json!({
        "GameType": "DarkSouls2",
        "Advertise": false,
        "ServerHostname": "127.0.0.1",
        "ServerPrivateHostname": "127.0.0.1",
        // Refuse two sessions from one Steam account. The session between two
        // players is peer to peer over Steam and keyed on the account's id, so
        // two instances sharing one account can never reach each other — and
        // allowing it only means the refusal arrives as a test that proves
        // nothing, hours later. The second Steam client exists for this.
        "AllowDuplicateSteamIds": false,
        // Upstream's welcome is an advert for another project, and it is the
        // first thing in the way every time the game starts. This replaces it
        // with the one thing worth reading at that moment: which server you
        // are on. An empty list would be quieter, but the game then says
        // "There is no new information", which is also what it says when it is
        // talking to FromSoftware's own servers — and telling those two apart
        // is the whole point of looking.
        "Announcements": [{
            "Header": "Servidor local — ds2os-dev",
            "Body": "\nVocê está no servidor desta máquina (127.0.0.1), não nos servidores oficiais.",
        }],
    })
}

/// Rewrites only the settings the harness owns, leaving the rest untouched.
/// Returns the keys it had to change.
pub fn apply_config(server: &ServerPaths) -> std::io::Result<Vec<String>> {
    let text = std::fs::read_to_string(&server.config)?;
    let mut config: Value = serde_json::from_str(&text)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;

    let mut changed = Vec::new();
    if let (Some(target), Some(required)) = (config.as_object_mut(), required_settings().as_object())
    {
        for (key, value) in required {
            if target.get(key) != Some(value) {
                target.insert(key.clone(), value.clone());
                changed.push(key.clone());
            }
        }
    }

    if !changed.is_empty() {
        std::fs::write(&server.config, serde_json::to_vec_pretty(&config)?)?;
    }
    Ok(changed)
}

pub fn config_ok(server: &ServerPaths) -> bool {
    let Ok(text) = std::fs::read_to_string(&server.config) else {
        return false;
    };
    let Ok(config) = serde_json::from_str::<Value>(&text) else {
        return false;
    };
    required_settings()
        .as_object()
        .map(|required| required.iter().all(|(k, v)| config.get(k) == Some(v)))
        .unwrap_or(false)
}

/// Starts the server if it is not already up, and waits for it to listen.
pub fn up(environment: &Environment) -> Result<ServerStatus, String> {
    let server = environment
        .server
        .as_ref()
        .ok_or("o servidor não está compilado; rode `ds2os-dev doctor`")?;

    if let Some(pid) = proc::running(&paths::server_pid(), "Server") {
        return Ok(status_for(server, Some(pid)));
    }

    // The server generates its keypair and config on first run, so a fresh
    // checkout has to be started once before there is anything to configure.
    if !server.config.is_file() {
        println!("  primeira execução: gerando config e chaves");
        start(server)?;
        wait_until_listening(Duration::from_secs(60))?;
        if let Some(pid) = proc::running(&paths::server_pid(), "Server") {
            proc::stop(pid);
        }
    }

    let changed = apply_config(server).map_err(|e| format!("não consegui ajustar a config: {e}"))?;
    if !changed.is_empty() {
        println!("  config ajustada: {}", changed.join(", "));
    }

    start(server)?;
    wait_until_listening(Duration::from_secs(60))?;

    Ok(status_for(server, proc::running(&paths::server_pid(), "Server")))
}

fn start(server: &ServerPaths) -> Result<(), String> {
    paths::ensure_dirs().map_err(|e| e.to_string())?;
    proc::spawn(
        &server.binary,
        &[],
        &server.working_dir,
        &[],
        &paths::server_log(),
        &paths::server_pid(),
        true,
    )
    .map(|_| ())
    .map_err(|e| format!("não consegui iniciar o servidor: {e}"))
}

/// Whether the pid in `pid_file` still exists at all, whatever it is running.
fn started_or_starting(pid_file: &std::path::Path) -> bool {
    let Ok(text) = std::fs::read_to_string(pid_file) else {
        return false;
    };
    let Ok(pid) = text.trim().parse::<u32>() else {
        return false;
    };
    std::fs::metadata(format!("/proc/{pid}")).is_ok()
}

fn wait_until_listening(timeout: Duration) -> Result<(), String> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if proc::tcp_port_busy(50050) && proc::tcp_port_busy(50000) {
            return Ok(());
        }
        // Only a process that is really gone counts as dead. Reading the
        // command line of one that is still starting can come back empty
        // while it execs, and treating that as death reported a server that
        // had in fact come up and was serving.
        if !started_or_starting(&paths::server_pid()) {
            return Err(format!(
                "o servidor morreu ao iniciar; veja {}",
                paths::server_log().display()
            ));
        }
        crate::control::sleep(Duration::from_millis(400))?;
    }
    Err(format!(
        "o servidor não abriu as portas em {}s; veja {}",
        timeout.as_secs(),
        paths::server_log().display()
    ))
}

pub fn down() -> bool {
    match proc::running(&paths::server_pid(), "Server") {
        Some(pid) => proc::stop(pid),
        None => true,
    }
}

pub fn status(environment: &Environment) -> ServerStatus {
    let pid = proc::running(&paths::server_pid(), "Server");
    match &environment.server {
        Some(server) => status_for(server, pid),
        None => ServerStatus {
            running: pid.is_some(),
            pid,
            ports_listening: listening_ports(),
            players: players_from_log(),
            config_ok: false,
            log: paths::server_log().display().to_string(),
        },
    }
}

fn status_for(server: &ServerPaths, pid: Option<u32>) -> ServerStatus {
    ServerStatus {
        running: pid.is_some(),
        pid,
        ports_listening: listening_ports(),
        players: players_from_log(),
        config_ok: config_ok(server),
        log: paths::server_log().display().to_string(),
    }
}

fn listening_ports() -> Vec<String> {
    PORTS
        .iter()
        .filter(|(_, port)| proc::tcp_port_busy(*port))
        .map(|(name, port)| format!("{name}:{port}"))
        .collect()
}

/// The heartbeat line the server prints every 30 seconds carries the player
/// count, which is the cheapest way to see whether a game actually joined.
fn players_from_log() -> Option<u32> {
    let text = crate::logs::read_text(&paths::server_log()).ok()?;
    text.lines()
        .rev()
        .find_map(|line| {
            let index = line.find(" players |")?;
            line[..index].rsplit(char::is_whitespace).next()?.parse::<u32>().ok()
        })
}

/// The server's public key, which the game has to present byte for byte.
pub fn public_key(server: &ServerPaths) -> Result<String, String> {
    let raw = std::fs::read_to_string(&server.public_key)
        .map_err(|e| format!("não consegui ler {}: {e}", server.public_key.display()))?;
    ds2os_core::normalize_public_key(&raw)
        .ok_or_else(|| format!("{} não é uma chave RSA válida", server.public_key.display()))
}

/// One `Sign poll` line, sanitised:
/// `2026-09-14 17:14:58 | Log  | 3:Chico  | Sign poll: area 0x009d5170, ..., room for 20, 0 signs cached, sticky skipped.`
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SignPoll { pub at: String, pub player: String, pub cached: u64 }

pub fn parse_sign_poll(line: &str) -> Option<SignPoll> {
    let fields: Vec<&str> = line.split('|').map(str::trim).collect();
    let message = fields.iter().position(|f| f.starts_with("Sign poll:"))?;
    let (before, _) = fields[message].split_once(" signs cached")?;
    let cached = before.rsplit(", ").next()?.parse().ok()?;
    Some(SignPoll { at: fields.first()?.to_string(), player: fields.get(message.checked_sub(1)?)?.to_string(), cached })
}

/// Waits until the server's sign cache holds `signs` entries, by the `Sign poll`
/// lines it writes after this starts. The count is the whole cache, not one
/// player's; the line is throttled to one per player every ten seconds and only
/// written with `DS2_StickySigns`. Passes on the newest poll saying `signs`;
/// no poll at all by the deadline is inconclusive, never a pass.
pub fn wait_signs(signs: u64, timeout: Duration) -> Result<(), String> {
    let log = paths::server_log();
    let mut from = std::fs::metadata(&log).map(|m| m.len()).unwrap_or(0);
    let deadline = Instant::now() + timeout;
    let started = Instant::now();
    let mut latest: std::collections::BTreeMap<String, SignPoll> = Default::default();
    let mut newest: Option<SignPoll> = None;
    loop {
        crate::control::check()?;
        if let Ok((text, next)) = crate::logs::read_lines_from(&log, from) {
            for poll in text.lines().filter_map(parse_sign_poll) {
                latest.insert(poll.player.clone(), poll.clone());
                newest = Some(poll);
            }
            from = next;
        }
        let data = json!({"signs": signs, "newest": newest, "byPlayer": latest, "elapsedMs": started.elapsed().as_millis()});
        if newest.as_ref().is_some_and(|p| p.cached == signs) {
            crate::output::data(data);
            crate::output::line(format_args!("  {signs} sign(s) no cache, pelo poll de {}", newest.as_ref().map(|p| p.player.as_str()).unwrap_or("")));
            return Ok(());
        }
        if Instant::now() >= deadline {
            crate::output::data(data);
            return match &newest {
                Some(poll) => Err(format!("signs_remain: o último poll ({}, {}) ainda mostra {} sign(s) no cache", poll.player, poll.at, poll.cached)),
                None => {
                    crate::output::outcome("inconclusive");
                    Err(format!("inconclusive: nenhum Sign poll no log do servidor em {}s", timeout.as_secs()))
                }
            };
        }
        crate::control::sleep(Duration::from_millis(1000))?;
    }
}

#[cfg(test)]
mod sign_tests {
    use super::*;

    #[test]
    fn a_sign_poll_line_gives_the_player_and_the_cache_size() {
        // From server.log on 14/09, after crate::logs sanitised it.
        let line = "2026-09-14 17:14:58 | Log  | 3:Chico  | Sign poll: area 0x009d5170, activity area 103110, 27 search cells (first 0x00000000007ffc00), room for 20, 0 signs cached, sticky skipped.";
        assert_eq!(parse_sign_poll(line), Some(SignPoll { at: "2026-09-14 17:14:58".into(), player: "3:Chico".into(), cached: 0 }));
        let one = line.replace(", 0 signs cached", ", 1 signs cached");
        assert_eq!(parse_sign_poll(&one).map(|p| p.cached), Some(1));
        assert_eq!(parse_sign_poll("2026-09-14 17:14:58 | Log  | 3:Chico  | Sign 1000 created"), None);
    }
}
