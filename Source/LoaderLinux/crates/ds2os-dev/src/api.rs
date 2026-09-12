//! What the server already knows about the players.
//!
//! The game client keeps name, soul memory, covenant and death count somewhere
//! in its own memory, and finding each of them is an afternoon. The server has
//! all of it already - the clients tell it - and publishes it over the web UI.
//! So this asks the server instead of reverse engineering the client.
//!
//! Careful with what DS2 actually fills. `deathCount`, `souls` and
//! `multiplayCount` come from PlayerState methods this game never implements,
//! so they are always zero - a death has to be read from the warp log instead
//! (see `watch`). What is real here: name, soul level, soul memory, area, play
//! time, and the status line, which carries the effigy count that decides
//! whether a character can be summoned at all.
//!
//! Login is off unless the config carries a username and a password, so the
//! credentials are read from the server's own config rather than invented
//! here: whatever the server was started with is what works.

use std::path::Path;
use std::process::Command;

use crate::env::ServerPaths;

/// One player, as the server sees them.
#[derive(Debug, Clone)]
pub struct Player {
    pub name: String,
    pub player_id: u64,
    pub steam_id: String,
    pub soul_level: i64,
    pub souls: i64,
    pub soul_memory: i64,
    pub death_count: i64,
    pub multiplay_count: i64,
    pub covenant: String,
    pub status: String,
    pub location: String,
    pub play_time: String,
}

fn curl(args: &[&str]) -> Result<String, String> {
    let output = Command::new("curl")
        .arg("-s")
        .arg("-m")
        .arg("5")
        .args(args)
        .output()
        .map_err(|e| format!("não consegui rodar curl: {e}"))?;
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn credentials(server: &ServerPaths) -> Result<(String, String), String> {
    let raw = std::fs::read_to_string(&server.config)
        .map_err(|e| format!("não consegui ler {}: {e}", server.config.display()))?;
    let json: serde_json::Value =
        serde_json::from_str(&raw).map_err(|e| format!("config do servidor inválido: {e}"))?;

    let user = json
        .get("WebUIServerUsername")
        .and_then(|v| v.as_str())
        .unwrap_or("");
    let pass = json
        .get("WebUIServerPassword")
        .and_then(|v| v.as_str())
        .unwrap_or("");

    if user.is_empty() || pass.is_empty() {
        return Err(
            "o web-ui do servidor está sem usuário e senha, então o login está desligado; \
             preencha WebUIServerUsername e WebUIServerPassword no config e reinicie"
                .to_owned(),
        );
    }
    Ok((user.to_owned(), pass.to_owned()))
}

fn token(server: &ServerPaths, port: u16) -> Result<String, String> {
    let (user, pass) = credentials(server)?;
    let body = serde_json::json!({ "username": user, "password": pass }).to_string();
    let url = format!("http://127.0.0.1:{port}/auth");

    let reply = curl(&[
        "-X", "POST", &url, "-H", "Content-Type: application/json", "-d", &body,
    ])?;

    let json: serde_json::Value = serde_json::from_str(&reply)
        .map_err(|_| format!("o servidor não respondeu json ao login: {}", reply.trim()))?;
    json.get("token")
        .and_then(|v| v.as_str())
        .map(|s| s.to_owned())
        .ok_or_else(|| format!("login recusado: {}", reply.trim()))
}

fn text(value: &serde_json::Value, key: &str) -> String {
    value
        .get(key)
        .map(|v| match v {
            serde_json::Value::String(s) => s.clone(),
            other => other.to_string(),
        })
        .unwrap_or_default()
}

fn number(value: &serde_json::Value, key: &str) -> i64 {
    value.get(key).and_then(|v| v.as_i64()).unwrap_or(-1)
}

/// Everyone the server currently has connected.
pub fn players(server: &ServerPaths, port: u16) -> Result<Vec<Player>, String> {
    let token = token(server, port)?;
    let url = format!("http://127.0.0.1:{port}/players");
    let reply = curl(&[&url, "-H", &format!("Auth-Token: {token}")])?;

    let json: serde_json::Value = serde_json::from_str(&reply)
        .map_err(|_| format!("o servidor não respondeu json: {}", reply.trim()))?;

    let list = json
        .get("players")
        .and_then(|v| v.as_array())
        .ok_or_else(|| format!("resposta sem lista de jogadores: {}", reply.trim()))?;

    Ok(list
        .iter()
        .map(|p| Player {
            name: text(p, "characterName"),
            player_id: p.get("playerId").and_then(|v| v.as_u64()).unwrap_or(0),
            steam_id: text(p, "steamId"),
            soul_level: number(p, "soulLevel"),
            souls: number(p, "souls"),
            soul_memory: number(p, "soulMemory"),
            death_count: number(p, "deathCount"),
            multiplay_count: number(p, "multiplayCount"),
            covenant: text(p, "covenant"),
            status: text(p, "status"),
            location: text(p, "location"),
            play_time: text(p, "playTime"),
        })
        .collect())
}

/// The web UI port the server was configured with.
pub fn web_port(config: &Path) -> u16 {
    std::fs::read_to_string(config)
        .ok()
        .and_then(|raw| serde_json::from_str::<serde_json::Value>(&raw).ok())
        .and_then(|json| json.get("WebUIServerPort").and_then(|v| v.as_u64()))
        .unwrap_or(50005) as u16
}
