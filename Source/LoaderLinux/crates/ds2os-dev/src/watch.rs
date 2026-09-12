//! Noticing that something went wrong, while it is going wrong.
//!
//! A scripted test fails in two ways that look identical from the outside: the
//! thing under test did not work, or the character died on the way to it. Both
//! produce a log full of nothing. Telling them apart afterwards, by screenshot,
//! costs more than the test did - and it has already cost two afternoons here.
//!
//! So this watches the things that say what happened, together:
//!
//! - the server's death count per player, which is the only unambiguous "they
//!   died" the harness can get without another address hunt;
//! - where each player is, by area name, so a character that fell somewhere
//!   else announces itself;
//! - the warp log the injector writes, which names *why* the game moved
//!   someone - an ordinary death, a forced return home, or a guest arriving in
//!   a host's world.
//!
//! It prints a line only when something changes, so a quiet run stays quiet and
//! anything it does print is worth reading.

use std::collections::HashMap;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use crate::api::{self, Player};
use crate::env::Environment;

/// One thing worth interrupting for.
#[derive(Debug, Clone)]
pub struct Event {
    pub at: String,
    pub what: String,
}

fn stamp() -> String {
    // The server log is timestamped and this has to line up with it, so the
    // wall clock it is, seconds included.
    let output = std::process::Command::new("date")
        .arg("+%H:%M:%S")
        .output()
        .ok();
    output
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned())
        .unwrap_or_default()
}

struct Seen {
    deaths: i64,
    location: String,
    souls: i64,
}

/// Watches until `duration` runs out, calling `on_event` for each change.
pub fn run(
    environment: &Environment,
    duration: Duration,
    mut on_event: impl FnMut(Event),
) -> Result<(), String> {
    let server = environment
        .server
        .as_ref()
        .ok_or("o servidor não está compilado; rode `ds2os-dev doctor`")?;
    let port = api::web_port(&server.config);

    let mut seen: HashMap<String, Seen> = HashMap::new();
    let mut warp_at: HashMap<PathBuf, u64> = HashMap::new();
    let deadline = Instant::now() + duration;
    let mut first = true;

    loop {
        match api::players(server, port) {
            Ok(list) => {
                for player in &list {
                    note_player(&mut seen, player, first, &mut on_event);
                }
            }
            Err(error) if first => {
                // Said once. A watch that cannot reach the server is still
                // useful for the warp logs, and repeating the complaint every
                // two seconds would bury them.
                on_event(Event {
                    at: stamp(),
                    what: format!("sem o servidor: {error}"),
                });
            }
            Err(_) => {}
        }

        for install in &environment.installs {
            let path = install.game_dir.join("DS2_Seamless.log");
            note_warps(&path, install.account, &mut warp_at, first, &mut on_event);
        }

        first = false;
        if Instant::now() >= deadline {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(1500));
    }
}

fn note_player(
    seen: &mut HashMap<String, Seen>,
    player: &Player,
    first: bool,
    on_event: &mut impl FnMut(Event),
) {
    let key = player.name.clone();
    let previous = seen.insert(
        key.clone(),
        Seen {
            deaths: player.death_count,
            location: player.location.clone(),
            souls: player.souls,
        },
    );

    let Some(previous) = previous else {
        if !first {
            on_event(Event {
                at: stamp(),
                what: format!("{key} entrou, em {}", player.location),
            });
        }
        return;
    };

    if player.death_count > previous.deaths {
        on_event(Event {
            at: stamp(),
            what: format!(
                "MORREU: {key}, agora {} mortes, em {} (tinha {} almas)",
                player.death_count, player.location, previous.souls
            ),
        });
    }
    if player.location != previous.location {
        on_event(Event {
            at: stamp(),
            what: format!("{key} mudou de área: {} -> {}", previous.location, player.location),
        });
    }
}

fn note_warps(
    path: &PathBuf,
    account: u8,
    warp_at: &mut HashMap<PathBuf, u64>,
    first: bool,
    on_event: &mut impl FnMut(Event),
) {
    let Ok(text) = std::fs::read_to_string(path) else {
        return;
    };
    let lines: Vec<&str> = text.lines().collect();
    let already = warp_at.get(path).copied().unwrap_or(0) as usize;
    warp_at.insert(path.clone(), lines.len() as u64);

    if first || lines.len() <= already {
        return;
    }

    for line in &lines[already..] {
        let trimmed = line.trim();
        if let Some(reason) = motive(trimmed) {
            on_event(Event {
                at: stamp(),
                what: format!("conta {account}: {reason}"),
            });
        }
    }
}

/// Turns a warp log line into something worth printing, or nothing.
fn motive(line: &str) -> Option<String> {
    if !line.starts_with("warp ") {
        if line.starts_with("co-op:") {
            return Some("o hook trocou a volta para casa pela fogueira".to_owned());
        }
        return None;
    }

    let motive = field(line, "motivo=")?;
    let force = field(line, "forca=").unwrap_or_default();
    Some(match (motive.as_str(), force.as_str()) {
        ("1", _) => "morreu e foi para a última fogueira".to_owned(),
        ("4", "0") => "voltou forçado para o próprio mundo (fim de sessão)".to_owned(),
        ("4", "1") => "entrou no mundo do host".to_owned(),
        ("5", _) => "usou Homeward Bone".to_owned(),
        (other, _) => format!("warp de motivo {other}"),
    })
}

fn field(line: &str, key: &str) -> Option<String> {
    let at = line.find(key)? + key.len();
    Some(
        line[at..]
            .split_whitespace()
            .next()
            .unwrap_or_default()
            .to_owned(),
    )
}
