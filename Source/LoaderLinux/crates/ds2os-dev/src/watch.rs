//! Noticing that something went wrong, while it is going wrong.
//!
//! A scripted test fails in two ways that look identical from the outside: the
//! thing under test did not work, or the character died on the way to it. Both
//! produce a log full of nothing. Telling them apart afterwards, by screenshot,
//! costs more than the test did - and it has already cost two afternoons here.
//!
//! So this watches the things that say what happened, together:
//!
//! - every hook log on both installations and the server log, read with the
//!   same table `timeline` uses (`timeline::classify`): a death and what it
//!   cost, a cancelled or refused death, a respawn, a warp and why, a request
//!   to end the session, the channel's members, a crash, a sign, and each
//!   `RequestNotify*` the server receives;
//! - the API, for who is connected and in which area.
//!
//! The API's death counter is not one of them: DS2 never fills it, so it is
//! always `null`, and a death is only ever seen in the death hook's log.
//!
//! It prints a line only when something changes, so a quiet run stays quiet and
//! anything it does print is worth reading.

use std::collections::HashMap;
use std::os::unix::fs::MetadataExt;
use std::path::PathBuf;
use std::time::{Duration, Instant};

use crate::api::{self, Player};
use crate::env::Environment;
use crate::timeline;

/// One thing worth interrupting for.
#[derive(Debug, Clone, serde::Serialize)]
pub struct Event {
    /// Local wall time: the line's own clock when it has one, else when it was read.
    pub at: String,
    pub what: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub instance: Option<u8>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub kind: Option<&'static str>,
}

/// The kinds that interrupt. Order echoes and status blocks are the harness's
/// own traffic and stay out.
const WATCHED: [&str; 14] = ["death_cost", "respawn_done", "death_cancelled", "death_seen", "copy_refused",
    "session_end_request", "channel_members", "warp", "crash", "hook_boot", "server_notify", "sign", "disconnect", "rematch"];

fn stamp() -> String {
    // The server log is timestamped and this has to line up with it.
    let now = crate::output::now_ms() as i64;
    timeline::format_local(now)[11..19].to_owned()
}

fn plain(what: String) -> Event { Event { at: stamp(), what, source: None, instance: None, kind: None } }

struct Seen {
    location: String,
}

/// Reads what a log gained since the last look, reopening it when it was
/// replaced or truncated.
#[derive(Default)]
struct Tail { at: HashMap<PathBuf, (u64, u64)> }

impl Tail {
    /// Whole new lines; the first look only records where the file ends.
    fn read(&mut self, path: &PathBuf, sanitise: bool) -> Vec<String> {
        let Ok(meta) = std::fs::metadata(path) else { return vec![] };
        let (inode, length) = (meta.ino(), meta.len());
        let Some(&(seen_inode, offset)) = self.at.get(path) else {
            self.at.insert(path.clone(), (inode, length));
            return vec![];
        };
        let from = if seen_inode != inode || length < offset { 0 } else { offset };
        let result = if sanitise {
            crate::logs::read_lines_from(path, from)
        } else {
            read_raw_lines(path, from)
        };
        match result {
            Ok((text, next)) => {
                self.at.insert(path.clone(), (inode, next));
                text.lines().map(str::to_owned).collect()
            }
            Err(_) => vec![],
        }
    }
}

fn read_raw_lines(path: &PathBuf, from: u64) -> std::io::Result<(String, u64)> {
    use std::io::{Read, Seek, SeekFrom};
    let mut file = std::fs::File::open(path)?;
    file.seek(SeekFrom::Start(from))?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)?;
    let complete = bytes.iter().rposition(|b| *b == b'\n').map(|k| k + 1).unwrap_or(0);
    Ok((String::from_utf8_lossy(&bytes[..complete]).into_owned(), from + complete as u64))
}

/// The events in lines a log gained. Hook lines without a clock take the time they were read.
fn events_from(source: &str, instance: Option<u8>, lines: &[String]) -> Vec<Event> {
    let text = lines.join("\n");
    let now = crate::output::now_ms() as i64;
    let entries = match source {
        "server" => timeline::parse_server(&text),
        _ => timeline::parse_hook(&text, source, instance.unwrap_or(0), timeline::Anchor::EndsAt(now)),
    };
    entries.into_iter().filter(|e| e.kind.is_some_and(|k| WATCHED.contains(&k))).map(|e| {
        let what = match (e.kind, motive(&e.line)) {
            (Some("warp"), Some(reason)) => format!("{reason} ({})", e.line),
            _ => e.line.clone(),
        };
        Event {
            at: e.at.as_deref().map(|a| a[11..].to_owned()).unwrap_or_else(stamp),
            what: match instance { Some(i) => format!("conta {i}: {what}"), None => what },
            source: Some(source.to_owned()), instance, kind: e.kind,
        }
    }).collect()
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
    let mut tail = Tail::default();
    let deadline = Instant::now() + duration;
    let mut first = true;

    let mut server_available = None;
    loop {
        crate::control::check()?;
        match api::players(server, port) {
            Ok(list) => {
                if server_available == Some(false) { on_event(plain("servidor recuperado".into())); }
                server_available = Some(true);
                let gone: Vec<_> = seen.keys().filter(|id| !list.iter().any(|p| &p.steam_id == *id)).cloned().collect();
                for id in gone { seen.remove(&id); on_event(plain(format!("steam {id} desconectou"))); }
                for player in &list {
                    note_player(&mut seen, player, first, &mut on_event);
                }
            }
            Err(error) => {
                if server_available != Some(false) { on_event(plain(format!("servidor indisponível: {error}"))); }
                server_available = Some(false);
            }
        }

        let lines = tail.read(&crate::paths::server_log(), true);
        for event in events_from("server", None, &lines) { on_event(event); }
        for install in &environment.installs {
            for (source, file) in timeline::HOOK_LOGS {
                let lines = tail.read(&install.game_dir.join(file), false);
                for event in events_from(source, Some(install.account), &lines) { on_event(event); }
            }
        }

        first = false;
        if Instant::now() >= deadline {
            if server_available == Some(false) {
                crate::output::outcome("inconclusive");
                return Err("watch_incomplete: servidor indisponível ao terminar".into());
            }
            return Ok(());
        }
        crate::control::sleep(Duration::from_millis(1500))?;
    }
}

fn note_player(
    seen: &mut HashMap<String, Seen>,
    player: &Player,
    first: bool,
    on_event: &mut impl FnMut(Event),
) {
    let key = player.steam_id.clone();
    let previous = seen.insert(key.clone(), Seen { location: player.location.clone() });

    let Some(previous) = previous else {
        if !first {
            on_event(plain(format!("{key} entrou, em {}", player.location)));
        }
        return;
    };
    if player.location != previous.location {
        on_event(plain(format!("{key} mudou de área: {} -> {}", previous.location, player.location)));
    }
}

/// Why the game moved someone, from a `warp motivo=` line.
fn motive(line: &str) -> Option<String> {
    if !line.starts_with("warp motivo=") { return None; }
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
    Some(line[at..].split_whitespace().next().unwrap_or_default().to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn watch_reports_deaths_and_warps_but_not_order_echoes() {
        let lines = vec![
            "17:40:45.894  morte vista hp=0 max=854 matador=00000000 flags=00000000 causa=10".to_owned(),
            "17:42:07.575  === modo: observar ===".to_owned(),
        ];
        let events = events_from("death", Some(2), &lines);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].kind, Some("death_seen"));
        assert_eq!(events[0].at, "17:40:45.894");
        assert!(events[0].what.starts_with("conta 2: morte vista"));
        let warp = events_from("seamless", Some(2), &["  warp motivo=4 forca=0 tipo=0 mapa=0a1f0000".to_owned()]);
        assert!(warp[0].what.contains("fim de sessão"), "{}", warp[0].what);
    }

    #[test]
    fn a_replaced_log_is_read_from_the_top() {
        let dir = std::env::temp_dir().join(format!("ds2os-watch-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("DS2_Death.log");
        std::fs::write(&path, "17:00:00.000  morte vista hp=0\n").unwrap();
        let mut tail = Tail::default();
        assert!(tail.read(&path, false).is_empty());
        std::fs::write(&path, "17:00:00.000  morte vista hp=0\n17:00:01.000  morte vista hp=0\npartial").unwrap();
        assert_eq!(tail.read(&path, false).len(), 1);
        std::fs::remove_file(&path).unwrap();
        std::fs::write(&path, "17:00:02.000  custos da morte\n").unwrap();
        assert_eq!(tail.read(&path, false), vec!["17:00:02.000  custos da morte".to_owned()]);
        std::fs::remove_dir_all(&dir).ok();
    }
}
