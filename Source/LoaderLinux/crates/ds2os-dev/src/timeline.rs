//! One ordered timeline out of the server log, the hooks' logs on both
//! installations and the harness's own events.
//!
//! Every source keeps its own clock, and none of them is complete:
//!
//! - the server writes `YYYY-MM-DD HH:MM:SS`, local time, one-second resolution;
//! - `DS2_Death`, `DS2_Channel` and `DS2_Backread` write `HH:MM:SS.mmm`, local
//!   time with no date, so the date comes from the file's mtime walking
//!   backwards (or from a run's start walking forwards) and a clock that jumps
//!   back by more than twelve hours is midnight;
//! - `DS2_Session`, `DS2_Seamless`, `DS2_Respawn`, `DS2_Crash`, `DS2_Trace` and
//!   `DS2_Rematch` write no clock at all. Their lines have no place in a window
//!   of wall time and are left out of `--last`/`--since`, which says so; a
//!   `--run` keeps them, untimed, because the run's own capture bounds them;
//! - `events.jsonl` carries epoch milliseconds.
//!
//! Inside one server second the order is by source, and each entry says its
//! `resolutionMs`. Indented lines continue the entry above them.
//!
//! `classify` is the one table of what a line means; `watch` reads with it too.

use std::path::{Path, PathBuf};

use serde::Serialize;
use serde_json::{json, Value};

use crate::env::Environment;

/// Hook logs worth a timeline, by the name the timeline gives them. The timer,
/// MemProbe and area probes are left out: their clocks are process uptime or
/// absent, and they are volume, not events.
pub const HOOK_LOGS: [(&str, &str); 9] = [
    ("death", "DS2_Death.log"), ("channel", "DS2_Channel.log"), ("backread", "DS2_Backread.log"),
    ("session", "DS2_Session.log"), ("seamless", "DS2_Seamless.log"), ("respawn", "DS2_Respawn.log"),
    ("crash", "DS2_Crash.log"), ("trace", "DS2_Trace.log"), ("rematch", "DS2_Rematch.log"),
];

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Entry {
    /// Epoch milliseconds; `None` for a source without a clock.
    pub at_ms: Option<i64>,
    /// The same instant as local time, `YYYY-MM-DD HH:MM:SS.mmm`.
    pub at: Option<String>,
    pub resolution_ms: u32,
    pub source: String,
    pub instance: Option<u8>,
    pub kind: Option<&'static str>,
    pub line: String,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub more: Vec<String>,
}

// ---- local time -----------------------------------------------------------

/// Epoch milliseconds of a local date and time, through the C library, so
/// daylight saving and the machine's zone are whatever the logs used.
pub fn local_ms(year: i32, month: u32, day: u32, seconds_of_day: i64, millis: i64) -> Option<i64> {
    // SAFETY: `tm` is plain data; mktime only reads and normalises it.
    unsafe {
        let mut tm: libc::tm = std::mem::zeroed();
        tm.tm_year = year - 1900;
        tm.tm_mon = month as i32 - 1;
        tm.tm_mday = day as i32;
        tm.tm_hour = 0;
        tm.tm_min = 0;
        tm.tm_sec = seconds_of_day as i32;
        tm.tm_isdst = -1;
        let t = libc::mktime(&mut tm);
        (t != -1).then_some(t as i64 * 1000 + millis)
    }
}

/// The local calendar date and time of day of an epoch millisecond.
pub fn local_parts(ms: i64) -> (i32, u32, u32, i64) {
    // SAFETY: localtime_r writes into the `tm` it is given.
    unsafe {
        let t: libc::time_t = ms.div_euclid(1000) as libc::time_t;
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&t, &mut tm);
        (tm.tm_year + 1900, (tm.tm_mon + 1) as u32, tm.tm_mday as u32,
            tm.tm_hour as i64 * 3600 + tm.tm_min as i64 * 60 + tm.tm_sec as i64)
    }
}

pub fn format_local(ms: i64) -> String {
    let (y, mo, d, s) = local_parts(ms);
    format!("{y:04}-{mo:02}-{d:02} {:02}:{:02}:{:02}.{:03}", s / 3600, s / 60 % 60, s % 60, ms.rem_euclid(1000))
}

/// Shifts a date by whole days, through mktime's normalisation.
fn day_ms(date: (i32, u32, u32), offset_days: i64, seconds_of_day: i64, millis: i64) -> Option<i64> {
    local_ms(date.0, date.1, date.2, offset_days * 86400 + seconds_of_day, millis)
}

// ---- parsing --------------------------------------------------------------

/// `HH:MM:SS.mmm  ` at the start of a hook line: seconds of day, millis, rest.
fn hook_clock(line: &str) -> Option<(i64, i64, &str)> {
    let b = line.as_bytes();
    if b.len() < 12 || b[2] != b':' || b[5] != b':' || b[8] != b'.' { return None; }
    let n = |r: std::ops::Range<usize>| line.get(r)?.parse::<i64>().ok();
    let (h, m, s, ms) = (n(0..2)?, n(3..5)?, n(6..8)?, n(9..12)?);
    (h < 24 && m < 60 && s < 60).then(|| (h * 3600 + m * 60 + s, ms, line[12..].trim_start()))
}

/// `2026-09-14 17:38:22` at the start of a sanitised server line.
fn server_clock(line: &str) -> Option<i64> {
    let n = |r: std::ops::Range<usize>| line.get(r)?.parse::<i64>().ok();
    if line.len() < 19 || line.as_bytes()[4] != b'-' || line.as_bytes()[13] != b':' { return None; }
    local_ms(n(0..4)? as i32, n(5..7)? as u32, n(8..10)? as u32, n(11..13)? * 3600 + n(14..16)? * 60 + n(17..19)?, 0)
}

/// Where a date-less hook clock gets its date from.
#[derive(Debug, Clone, Copy)]
pub enum Anchor {
    /// The last line was written at or before this instant (the file's mtime).
    EndsAt(i64),
    /// The first line was written at or after this instant (a run's start).
    StartsAt(i64),
}

pub fn parse_hook(text: &str, source: &str, instance: u8, anchor: Anchor) -> Vec<Entry> {
    let mut entries: Vec<Entry> = Vec::new();
    let mut clocks: Vec<(usize, i64, i64)> = Vec::new();
    for raw in text.lines() {
        if raw.trim().is_empty() { continue; }
        if let Some((seconds, millis, rest)) = hook_clock(raw) {
            clocks.push((entries.len(), seconds, millis));
            entries.push(entry(None, 1, source, Some(instance), rest));
        } else if raw.starts_with(char::is_whitespace) && entries.last().is_some_and(|e| e.resolution_ms == 1) {
            entries.last_mut().unwrap().more.push(raw.trim().to_owned());
        } else {
            entries.push(entry(None, 0, source, Some(instance), raw.trim()));
        }
    }
    const HALF_DAY: i64 = 43200;
    match anchor {
        Anchor::EndsAt(end) => {
            let (y, mo, d, end_seconds) = local_parts(end);
            // A last clock later in the day than the mtime was written yesterday.
            let mut day = 0i64;
            let mut later: Option<i64> = None;
            for &(index, seconds, millis) in clocks.iter().rev() {
                match later {
                    None if seconds > end_seconds + 60 => day -= 1,
                    Some(next) if seconds > next + HALF_DAY => day -= 1,
                    _ => {}
                }
                later = Some(seconds);
                entries[index].at_ms = day_ms((y, mo, d), day, seconds, millis);
            }
        }
        Anchor::StartsAt(start) => {
            let (y, mo, d, start_seconds) = local_parts(start);
            let mut day = 0i64;
            let mut earlier: Option<i64> = None;
            for &(index, seconds, millis) in &clocks {
                match earlier {
                    None if seconds + HALF_DAY < start_seconds => day += 1,
                    Some(previous) if seconds + HALF_DAY < previous => day += 1,
                    _ => {}
                }
                earlier = Some(seconds);
                entries[index].at_ms = day_ms((y, mo, d), day, seconds, millis);
            }
        }
    }
    for e in &mut entries { e.at = e.at_ms.map(format_local); }
    entries
}

pub fn parse_server(text: &str) -> Vec<Entry> {
    let mut entries: Vec<Entry> = Vec::new();
    for raw in text.lines() {
        if raw.trim().is_empty() { continue; }
        match server_clock(raw) {
            Some(ms) => {
                let mut e = entry(Some(ms), 1000, "server", None, raw.get(19..).unwrap_or("").trim_start_matches([' ', '|']).trim());
                e.at = Some(format_local(ms));
                entries.push(e);
            }
            None => if let Some(last) = entries.last_mut() { last.more.push(raw.trim().to_owned()) },
        }
    }
    entries
}

pub fn parse_events(text: &str, run: &str) -> Vec<Entry> {
    text.lines().filter_map(|l| serde_json::from_str::<Value>(l).ok()).filter_map(|v| {
        let ms = v.get("atMs")?.as_i64()?;
        let kind = v.get("kind")?.as_str()?.to_owned();
        let data = v.get("data").cloned().unwrap_or(Value::Null);
        let mut compact = data.to_string();
        if compact.len() > 400 { let mut cut = 400; while !compact.is_char_boundary(cut) { cut -= 1; } compact.truncate(cut); compact.push('…'); }
        Some(Entry { at_ms: Some(ms), at: Some(format_local(ms)), resolution_ms: 1, source: "harness".into(),
            instance: data.get("instance").and_then(Value::as_u64).and_then(|i| u8::try_from(i).ok()),
            kind: Some("harness"), line: format!("{kind} {compact} (run {run})"), more: vec![] })
    }).collect()
}

fn entry(at_ms: Option<i64>, resolution_ms: u32, source: &str, instance: Option<u8>, line: &str) -> Entry {
    Entry { at_ms, at: None, resolution_ms, source: source.to_owned(), instance, kind: classify(source, line), line: line.to_owned(), more: vec![] }
}

// ---- meaning --------------------------------------------------------------

/// What a line means, by where it came from. `None` is a line with no event in it.
pub fn classify(source: &str, line: &str) -> Option<&'static str> {
    let has = |needle: &str| line.contains(needle);
    if line.starts_with("=== ds2os") { return Some("hook_boot"); }
    match source {
        "server" => {
            if has("Notify RequestNotify") { Some("server_notify") }
            else if has("First DS2_Frpg2RequestMessage.RequestNotify") { Some("server_notify_census") }
            else if has("Sign poll:") { Some("sign_poll") }
            else if (line.contains("| Sign ") || has("Summoning sign") || has("Summoning sticky sign") || has("Rejecting summon"))
                && !has("Sign poll") { Some("sign") }
            else if has("Client timed out") || has("Disconnecting client") || has("Connection closed") { Some("disconnect") }
            else if has("has logged in as player") { Some("login") }
            else if has("Rematch:") { Some("rematch") }
            else { None }
        }
        "death" => {
            if has("custos da morte") { Some("death_cost") }
            else if has("renascer concluido") { Some("respawn_done") }
            else if line.starts_with("renascer:") { Some("respawn_step") }
            else if has("morte da copia RECUSADA") { Some("copy_refused") }
            else if has("morte CANCELADA") { Some("death_cancelled") }
            else if line.starts_with("morte vista") { Some("death_seen") }
            else if line.starts_with("=== modo") || line.starts_with("=== cobranca") || line.starts_with("=== nao entendi") { Some("death_order") }
            else { None }
        }
        "session" => {
            if has("fim de sessao") { Some("session_end_request") }
            else if has("host: estado") || has("host: controlador") { Some("host_state") }
            else if line.starts_with("===") { Some("session_order") }
            else { None }
        }
        "channel" => {
            if has("fogueira anunciada") { Some("channel_announce") }
            else if has("fogueira do host recebida") { Some("channel_received") }
            else if line.starts_with("sessao ") { Some("channel_members") }
            else if line.starts_with("=== canal ") { Some("channel_status") }
            else { None }
        }
        "backread" => {
            if line.starts_with("mapa ") && has(": estado ") { Some("backread_state") }
            else if line.starts_with("mapa ") && has("solto") { Some("backread_release") }
            else if has("mantido") { Some("backread_keep") }
            else if line.starts_with("foco no mapa") { Some("backread_focus") }
            else if line.starts_with("=== pedido") || line.starts_with("=== backread") { Some("backread_order") }
            else { None }
        }
        "seamless" => {
            if line.starts_with("warp motivo=") { Some("warp") }
            else if line.starts_with("warp aceito") { Some("warp_accepted") }
            else if line.starts_with("co-op:") || line.starts_with("===") { Some("seamless_order") }
            else { None }
        }
        "respawn" => line.starts_with("===").then_some("respawn_order"),
        "crash" => Some("crash"),
        "trace" => {
            if has("alcancado") || has("escreveu em") { Some("trace_hit") }
            else if line.starts_with("===") { Some("trace_order") }
            else { None }
        }
        "rematch" => Some("rematch"),
        _ => None,
    }
}

// ---- collection -----------------------------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct Window { pub from_ms: i64, pub to_ms: i64 }

/// `10m`, `90s`, `2h`.
pub fn parse_duration(text: &str) -> Result<i64, String> {
    let text = text.trim();
    let (number, unit) = text.split_at(text.find(|c: char| !c.is_ascii_digit()).unwrap_or(text.len()));
    let n: i64 = number.parse().map_err(|_| format!("invalid_duration: {text:?}; use 90s, 10m ou 2h"))?;
    Ok(n * match unit { "s" | "" => 1000, "m" => 60_000, "h" => 3_600_000, _ => return Err(format!("invalid_duration: {text:?}; use 90s, 10m ou 2h")) })
}

/// `HH:MM[:SS]` today (yesterday when that is still ahead), or `YYYY-MM-DD HH:MM:SS`.
pub fn parse_since(text: &str, now_ms: i64) -> Result<i64, String> {
    let text = text.trim();
    if let Some(ms) = server_clock(text) { return Ok(ms); }
    let parts: Vec<i64> = text.split(':').map(|p| p.parse().ok()).collect::<Option<_>>()
        .ok_or_else(|| format!("invalid_since: {text:?}; use HH:MM:SS ou 'AAAA-MM-DD HH:MM:SS'"))?;
    let seconds = match parts.as_slice() { [h, m] => h * 3600 + m * 60, [h, m, s] => h * 3600 + m * 60 + s,
        _ => return Err(format!("invalid_since: {text:?}")) };
    let (y, mo, d, _) = local_parts(now_ms);
    let today = day_ms((y, mo, d), 0, seconds, 0).ok_or("invalid_since: data local")?;
    Ok(if today > now_ms { day_ms((y, mo, d), -1, seconds, 0).unwrap_or(today) } else { today })
}

fn mtime_ms(path: &Path) -> Option<i64> {
    let modified = std::fs::metadata(path).ok()?.modified().ok()?;
    Some(modified.duration_since(std::time::UNIX_EPOCH).ok()?.as_millis() as i64)
}

fn run_start_ms(id: &str) -> Option<i64> {
    id.split('-').next()?.parse::<i128>().ok().map(|ns| (ns / 1_000_000) as i64)
}

pub struct Collected { pub entries: Vec<Entry>, pub sources: Vec<Value>, pub notes: Vec<String>, pub window: Option<Window> }

/// The live logs, over a window of wall time.
pub fn collect_window(env: &Environment, window: Window) -> Collected {
    let mut entries = Vec::new();
    let mut sources = Vec::new();
    let mut untimed = Vec::new();
    let server = crate::paths::server_log();
    if let Ok(text) = crate::logs::read_text(&server) {
        let parsed = parse_server(&text);
        sources.push(json!({"source": "server", "path": server, "lines": parsed.len()}));
        entries.extend(parsed);
    }
    for install in &env.installs {
        for (source, file) in HOOK_LOGS {
            let path = install.game_dir.join(file);
            let (Ok(text), Some(end)) = (std::fs::read_to_string(&path), mtime_ms(&path)) else { continue };
            let parsed = parse_hook(&text, source, install.account, Anchor::EndsAt(end));
            let timed = parsed.iter().filter(|e| e.at_ms.is_some()).count();
            if timed == 0 && !parsed.is_empty() && end >= window.from_ms { untimed.push(format!("{source}@{}", install.account)); }
            sources.push(json!({"source": source, "instance": install.account, "path": path, "lines": parsed.len(), "timed": timed}));
            entries.extend(parsed);
        }
    }
    // Runs whose id (the epoch nanosecond they began) falls near the window.
    if let Ok(dirs) = std::fs::read_dir(crate::paths::state_dir().join("runs")) {
        for dir in dirs.flatten() {
            let id = dir.file_name().to_string_lossy().to_string();
            let Some(start) = run_start_ms(&id) else { continue };
            if start > window.to_ms || mtime_ms(&dir.path().join("events.jsonl")).is_some_and(|m| m < window.from_ms) { continue; }
            if let Ok(text) = std::fs::read_to_string(dir.path().join("events.jsonl")) { entries.extend(parse_events(&text, &id)); }
        }
    }
    entries.retain(|e| e.at_ms.is_some_and(|ms| ms >= window.from_ms && ms <= window.to_ms));
    let mut notes = Vec::new();
    if !untimed.is_empty() {
        notes.push(format!("untimed_sources: {} escreveram na janela sem relógio e ficam de fora; `timeline --run` os inclui", untimed.join(", ")));
    }
    Collected { entries, sources, notes, window: Some(window) }
}

/// What one run captured: its own `logs/` deltas and `events.jsonl`.
pub fn collect_run(id: &str) -> Result<Collected, String> {
    let dir = crate::paths::state_dir().join("runs").join(id);
    let events = std::fs::read_to_string(dir.join("events.jsonl")).map_err(|e| format!("run_missing: {}: {e}", dir.display()))?;
    let start = run_start_ms(id).ok_or_else(|| format!("run_missing: id inválido {id}"))?;
    let mut entries = parse_events(&events, id);
    let end = entries.iter().filter_map(|e| e.at_ms).max().unwrap_or(start);
    let mut sources = Vec::new();
    let mut notes = Vec::new();
    let logs = dir.join("logs");
    for file in std::fs::read_dir(&logs).map(|d| d.flatten().collect::<Vec<_>>()).unwrap_or_default() {
        let name = file.file_name().to_string_lossy().to_string();
        let path: PathBuf = file.path();
        let parsed = if name == "server.log" {
            crate::logs::read_text(&path).map(|t| parse_server(&t)).unwrap_or_default()
        } else if let Some((instance, hook)) = name.strip_prefix("instance-").and_then(|r| r.split_once('-')) {
            let (Ok(instance), Some((source, _))) = (instance.parse::<u8>(), HOOK_LOGS.iter().find(|(_, f)| *f == hook)) else { continue };
            std::fs::read_to_string(&path).map(|t| parse_hook(&t, source, instance, Anchor::StartsAt(start))).unwrap_or_default()
        } else { continue };
        sources.push(json!({"source": name, "lines": parsed.len()}));
        entries.extend(parsed);
    }
    if sources.is_empty() { notes.push("no_captured_logs: a execução não guardou logs/".into()); }
    if entries.iter().any(|e| e.at_ms.is_none()) {
        notes.push("untimed_lines: linhas sem relógio vêm no fim, com at null, dentro da janela da execução".into());
    }
    Ok(Collected { entries, sources, notes, window: Some(Window { from_ms: start, to_ms: end }) })
}

/// Stable sort by time; ties keep the source's own order, and untimed entries go last.
pub fn order(entries: &mut [Entry]) {
    entries.sort_by_key(|e| (e.at_ms.is_none(), e.at_ms.unwrap_or(0)));
}

pub struct Filter { pub instance: Option<u8>, pub kinds: Vec<String>, pub all: bool }

pub fn keep(entry: &Entry, filter: &Filter) -> bool {
    if let (Some(wanted), Some(instance)) = (filter.instance, entry.instance) { if wanted != instance { return false; } }
    if !filter.kinds.is_empty() { return entry.kind.is_some_and(|k| filter.kinds.iter().any(|w| w == k)); }
    filter.all || entry.kind.is_some_and(|k| k != "harness" && k != "sign_poll" && k != "channel_status")
}

pub fn command(env: &Environment, last: Option<String>, since: Option<String>, run: Option<String>, filter: Filter, limit: usize) -> Result<(), String> {
    let now = crate::output::now_ms() as i64;
    let mut collected = match (last, since, run) {
        (_, _, Some(id)) => collect_run(&id)?,
        (Some(last), None, None) => collect_window(env, Window { from_ms: now - parse_duration(&last)?, to_ms: now }),
        (None, Some(since), None) => collect_window(env, Window { from_ms: parse_since(&since, now)?, to_ms: now }),
        (None, None, None) => collect_window(env, Window { from_ms: now - 600_000, to_ms: now }),
        (Some(_), Some(_), None) => return Err("invalid_window: use --last ou --since, não os dois".into()),
    };
    order(&mut collected.entries);
    let total = collected.entries.len();
    let mut kept: Vec<Entry> = collected.entries.into_iter().filter(|e| keep(e, &filter)).collect();
    let mut counts = std::collections::BTreeMap::<&str, usize>::new();
    for e in &kept { *counts.entry(e.kind.unwrap_or("none")).or_default() += 1; }
    let dropped = kept.len().saturating_sub(limit);
    if dropped > 0 {
        kept.drain(..dropped);
        collected.notes.push(format!("limited: as {dropped} entradas mais antigas ficaram de fora; use --limit"));
    }
    for e in &kept {
        crate::output::line(format_args!("{}  {:<9}{} {:<20} {}{}", e.at.as_deref().map(|a| &a[11..]).unwrap_or("  (sem hora)  "),
            e.source, e.instance.map(|i| format!("@{i}")).unwrap_or_else(|| "  ".into()), e.kind.unwrap_or("-"), e.line,
            if e.more.is_empty() { String::new() } else { format!(" [+{} linha(s)]", e.more.len()) }));
    }
    for note in &collected.notes { crate::output::line(format_args!("nota: {note}")); }
    crate::output::data(json!({
        "from": collected.window.map(|w| format_local(w.from_ms)), "to": collected.window.map(|w| format_local(w.to_ms)),
        "entries": kept, "counts": counts, "read": total, "sources": collected.sources, "notes": collected.notes,
    }));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // From DS2_Death.log on 14/09, a respawn in a session.
    const DEATH: &str = "13:34:18.348  custos da morte (papel 0, convidado 0): almas 0 -> 0 (registradas: 0 almas para a mancha, 0 perdidas da anterior); hollow aplicado
13:34:18.349  morte CANCELADA #1 (seguida 1) hp=0 -> 915 matador=00000000 flags=00000000 causa=10
13:34:19.032  renascer concluido em 1 quadros: +0x4c0 0000000000000000 -> 0000000000000000, camera de queda nao estava ligada, hp 915 -> 869
=== ds2os morte: modo observar ===
";

    fn at(year: i32, month: u32, day: u32, h: i64, m: i64, s: i64, ms: i64) -> i64 {
        local_ms(year, month, day, h * 3600 + m * 60 + s, ms).unwrap()
    }

    #[test]
    fn hook_lines_take_the_date_of_the_file_and_their_meaning() {
        let entries = parse_hook(DEATH, "death", 1, Anchor::EndsAt(at(2026, 9, 14, 13, 40, 0, 0)));
        let kinds: Vec<_> = entries.iter().map(|e| e.kind).collect();
        assert_eq!(kinds, [Some("death_cost"), Some("death_cancelled"), Some("respawn_done"), Some("hook_boot")]);
        assert_eq!(entries[0].at.as_deref(), Some("2026-09-14 13:34:18.348"));
        assert_eq!(entries[2].at_ms, Some(at(2026, 9, 14, 13, 34, 19, 32)));
        // The boot banner has no clock and is not glued to the line above it.
        assert_eq!(entries[3].at_ms, None);
    }

    #[test]
    fn a_clock_that_goes_back_twelve_hours_is_midnight() {
        let text = "23:59:59.900  morte vista hp=0\n00:00:00.100  morte vista hp=0\n";
        let back = parse_hook(text, "death", 2, Anchor::EndsAt(at(2026, 9, 15, 0, 0, 1, 0)));
        assert_eq!(back[0].at.as_deref(), Some("2026-09-14 23:59:59.900"));
        assert_eq!(back[1].at.as_deref(), Some("2026-09-15 00:00:00.100"));
        let forward = parse_hook(text, "death", 2, Anchor::StartsAt(at(2026, 9, 14, 23, 59, 0, 0)));
        assert_eq!(forward[1].at.as_deref(), Some("2026-09-15 00:00:00.100"));
        // A file last written just after midnight whose last line is from before it.
        let stale = parse_hook("23:59:58.000  morte vista hp=0\n", "death", 2, Anchor::EndsAt(at(2026, 9, 15, 0, 0, 2, 0)));
        assert_eq!(stale[0].at.as_deref(), Some("2026-09-14 23:59:58.000"));
    }

    #[test]
    fn indented_lines_continue_the_entry_above() {
        let text = "17:44:38.231  === canal 7: polls=7812 estranhos=0 enviados=36 falhas=0 recebidos=0 recusados=0 eu=011000010afd1a3a ===\n    sessao 00007FFFFE5BFA00 vista ha 219480 ms: 0 membros: nenhum\n    do host: nada recebido\n";
        let entries = parse_hook(text, "channel", 1, Anchor::EndsAt(at(2026, 9, 14, 17, 45, 0, 0)));
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].kind, Some("channel_status"));
        assert_eq!(entries[0].more.len(), 2);
    }

    #[test]
    fn server_lines_are_sanitised_and_classified() {
        // Raw bytes as the server writes them, separators not valid UTF-8.
        let raw = b"2026-09-14 17:40:58 \xb3 Log     \xb3 3:Chico                             \xb3 Notify RequestNotifyLeaveSession: fields 0 2 0 1.\n";
        let dir = std::env::temp_dir().join(format!("ds2os-timeline-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("server.log");
        std::fs::write(&path, raw).unwrap();
        let entries = parse_server(&crate::logs::read_text(&path).unwrap());
        std::fs::remove_dir_all(&dir).ok();
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].kind, Some("server_notify"));
        assert_eq!(entries[0].at_ms, Some(at(2026, 9, 14, 17, 40, 58, 0)));
        assert_eq!(entries[0].resolution_ms, 1000);
        assert!(entries[0].line.contains("3:Chico"), "{}", entries[0].line);
        assert_eq!(classify("server", "Log | 3:Chico | First DS2_Frpg2RequestMessage.RequestNotifyLeaveSession."), Some("server_notify_census"));
        assert_eq!(classify("server", "Log | 3:Chico | Sign 1001 created: type 1, area 0x009d5170, cell 0x00000000007ffc00."), Some("sign"));
        assert_eq!(classify("server", "Log | 1:Samuel | Sign poll: area 0x009d5170, activity area 103110, 27 search cells, room for 19, 1 signs cached, sticky skipped."), Some("sign_poll"));
    }

    #[test]
    fn the_table_knows_the_other_hooks() {
        assert_eq!(classify("death", "morte da copia RECUSADA #1 (seguida 1) controlador 00007FFFEB897D50"), Some("copy_refused"));
        assert_eq!(classify("session", "  fim de sessao pedido sessao=00007FFF papel=2 estado=3 motivo=4 motivo_anterior=0 de=+0x1"), Some("session_end_request"));
        assert_eq!(classify("session", "host: estado -> 16 (0x10) no quadro 1234"), Some("host_state"));
        assert_eq!(classify("channel", "fogueira anunciada na sessao 00007FFFFE5BFA00: mapa 0a1f0000 tipo 0 id 00007ba7 (seq 3)"), Some("channel_announce"));
        assert_eq!(classify("backread", "mapa 0a120000: estado 2 -> 5, 850 ms depois do pedido"), Some("backread_state"));
        assert_eq!(classify("seamless", "warp motivo=1 forca=0 tipo=0 mapa=0a1f0000"), Some("warp"));
        assert_eq!(classify("death", "slot +0x10 controlador=00007FFFE8290380 personagem=00007FFFE8295B00 local=0"), None);
    }

    #[test]
    fn events_keep_their_epoch_and_instance() {
        let line = r#"{"atMs":1789418437769,"kind":"death_orders","data":{"instance":2,"orders":["observe"],"ok":true}}"#;
        let entries = parse_events(line, "1789418437769704278-509903");
        assert_eq!(entries[0].at_ms, Some(1789418437769));
        assert_eq!(entries[0].instance, Some(2));
        assert!(entries[0].line.starts_with("death_orders {"));
    }

    #[test]
    fn windows_parse_durations_and_clock_times() {
        assert_eq!(parse_duration("10m"), Ok(600_000));
        assert_eq!(parse_duration("90s"), Ok(90_000));
        assert!(parse_duration("ten").is_err());
        let now = at(2026, 9, 14, 17, 0, 0, 0);
        assert_eq!(parse_since("16:30", now), Ok(at(2026, 9, 14, 16, 30, 0, 0)));
        // Still ahead today means yesterday.
        assert_eq!(parse_since("18:00:00", now), Ok(at(2026, 9, 13, 18, 0, 0, 0)));
        assert_eq!(parse_since("2026-09-14 12:00:00", now), Ok(at(2026, 9, 14, 12, 0, 0, 0)));
    }

    #[test]
    fn the_default_filter_hides_volume_but_not_events() {
        let filter = Filter { instance: Some(1), kinds: vec![], all: false };
        let mut e = entry(Some(0), 1, "death", Some(1), "custos da morte (papel 0, convidado 0)");
        assert!(keep(&e, &filter));
        e.instance = Some(2);
        assert!(!keep(&e, &filter));
        let poll = entry(Some(0), 1000, "server", None, "Log | 1:Samuel | Sign poll: area 0x0, 1 signs cached");
        assert!(!keep(&poll, &filter));
        assert!(keep(&poll, &Filter { instance: None, kinds: vec!["sign_poll".into()], all: false }));
    }
}
