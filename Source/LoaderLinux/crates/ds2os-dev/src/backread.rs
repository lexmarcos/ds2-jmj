//! `DS2_BackreadHook`: another map loaded beside the current one, and a
//! character moved into it without a warp.
//!
//! Every order is confirmed by its echo, and the echo only says the order was
//! read. What the order does arrives later, in lines of its own, and those are
//! what the commands wait on:
//!
//! - `load <map>` echoes `=== pedido: mapa X partes ... ===`; the map is in when
//!   the hook's own status lists it at `estado 5` (the log says
//!   `mapa X: estado 4 -> 5, N ms depois do pedido`);
//! - `focus <map> x y z` echoes `=== pedido: foco no mapa X em (...) ===`; the
//!   streamer has it when `foco no mapa X em (...): celula N (tentativa K)`
//!   names a cell other than -1 or -2 (-2 was the misaligned vec4 of 14/09).
//!
//! `goto-map` does the whole move and passes on the physics contact under the
//! character, not its coordinates: maps share coordinates (Heide's first
//! bonfire stands where Majula's sea rocks are), so a position proves nothing
//! about which map's ground is underneath. The contact handle at
//! `*(*(chr+0x100)+0x10)+0xe0` has the kind in its low nibble (7 collision,
//! 1 a map object) and the map index in bits 4..9, the same index the status
//! prints as `[i]`: measured 0xc7 on Heide ([12]) and 0x3c17 on Majula ([1]).

use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::{json, Value};

use crate::env::{Environment, Install};
use crate::hook_request;
use crate::probe::{self, Command, Reply};

const STEM: &str = "DS2_Backread";
const LOG: &str = "DS2_Backread.log";
const LOADED: u32 = 5;

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Owner { pub index: u32, pub map: String, pub state: u32, pub forced: u32 }

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status { pub owners: u32, pub requested: String, pub focus: Option<String>, pub maps: Vec<Owner> }

impl Status {
    pub fn owner(&self, map: &str) -> Option<&Owner> { self.maps.iter().find(|o| o.map == map) }
}

/// The status block starting at `lines[at]`:
///
/// ```text
/// === backread: 38 mapas; pedido 00000000 mascara 0000...; foco 00000000 ===
///     [12] mapa 0a1f0000 estado 5 forcado 0 quer 1: +10=... +20=...
/// ```
///
/// Older builds leave out `; foco` and write `partes` instead of `quer`.
pub fn parse_status_at(lines: &[&str], at: usize) -> Result<Status, String> {
    let header = lines[at];
    let malformed = || format!("malformed_answer: {header:?}");
    let words: Vec<&str> = header.split_whitespace().collect();
    let after = |key: &str| words.iter().position(|w| *w == key).and_then(|k| words.get(k + 1)).map(|w| w.trim_end_matches(';').to_owned());
    let owners = words.get(2).and_then(|w| w.parse().ok()).ok_or_else(malformed)?;
    let requested = after("pedido").ok_or_else(malformed)?;
    let focus = after("foco");
    let maps = lines[at + 1..].iter().map(|l| l.trim_start()).take_while(|l| l.starts_with('['))
        .filter_map(|l| {
            let w: Vec<&str> = l.split_whitespace().collect();
            let field = |key: &str| w.iter().position(|x| *x == key).and_then(|k| w.get(k + 1)).copied();
            Some(Owner {
                index: w.first()?.trim_matches(|c| c == '[' || c == ']').parse().ok()?,
                map: field("mapa")?.to_owned(),
                state: field("estado")?.parse().ok()?,
                forced: field("forcado")?.parse().ok()?,
            })
        }).collect();
    Ok(Status { owners, requested, focus, maps })
}

#[cfg(test)]
fn parse_status(appended: &str) -> Result<Option<Status>, String> {
    let lines = hook_request::lines(appended);
    match lines.iter().position(|l| l.starts_with("=== backread: ")) {
        Some(at) => parse_status_at(&lines, at).map(Some),
        None => Ok(None),
    }
}

/// `0a040000`, `0x0A040000` or `a040000`, as the hook prints it.
pub fn parse_map(text: &str) -> Result<String, String> {
    let value = u32::from_str_radix(text.trim().trim_start_matches("0x").trim_start_matches("0X"), 16)
        .map_err(|_| format!("invalid_map: {text:?}; use o id em hex, como 0a040000"))?;
    Ok(format!("{value:08x}"))
}

#[derive(Debug, Clone)]
pub enum Order { Load(String), Focus(String, [f32; 3]), Unfocus, Clear, Keep(i32, u32), Status }

impl Order {
    fn line(&self) -> String {
        match self {
            Order::Load(map) => format!("load {map}"),
            Order::Focus(map, [x, y, z]) => format!("focus {map} {x} {y} {z}"),
            Order::Unfocus => "unfocus".into(),
            Order::Clear => "clear".into(),
            Order::Keep(index, ms) => format!("keep {index} {ms}"),
            Order::Status => "status".into(),
        }
    }
    fn echoed(&self, line: &str) -> bool {
        match self {
            Order::Load(map) => line.starts_with(&format!("=== pedido: mapa {map} partes ")),
            Order::Focus(map, _) => line.starts_with(&format!("=== pedido: foco no mapa {map} em ")),
            Order::Unfocus => line == "=== pedido: sem foco ===",
            Order::Clear => line == "=== pedido: soltar ===",
            Order::Keep(index, ms) => line.starts_with(&format!("=== pedido: manter o mapa de indice {index} por {ms} ms")),
            Order::Status => line.starts_with("=== backread: "),
        }
    }
}

/// Sends the orders and waits for each echo, in order; the status, when one was asked, comes back.
pub fn send(install: &Install, orders: &[Order], timeout: Duration) -> Result<Option<Status>, String> {
    let body: String = orders.iter().map(|o| o.line() + "\n").collect();
    let result = hook_request::exchange(&install.game_dir, STEM, &body, timeout, |appended| {
        let lines = hook_request::lines(appended);
        let mut at = 0;
        let mut status = None;
        for order in orders {
            let Some(k) = lines[at..].iter().position(|l| order.echoed(l)) else { return Ok(None) };
            if matches!(order, Order::Status) { status = Some(parse_status_at(&lines, at + k)?); }
            at += k + 1;
        }
        Ok(Some(status))
    });
    crate::output::event("backread_orders", json!({"instance": install.account, "orders": orders.iter().map(Order::line).collect::<Vec<_>>(),
        "ok": result.is_ok(), "error": result.as_ref().err()}));
    result
}

pub fn status(install: &Install, timeout: Duration) -> Result<Status, String> {
    send(install, &[Order::Status], timeout)?.ok_or_else(|| "no_answer: status sem resposta".into())
}

/// The cell the streamer took for a focus on `map`: `Some(cell)` for each
/// attempt line, in order.
pub fn focus_cells(appended: &str, map: &str) -> Vec<i64> {
    let prefix = format!("foco no mapa {map} em ");
    hook_request::lines(appended).into_iter().filter(|l| l.starts_with(&prefix)).filter_map(|l| {
        let rest = l.split("celula ").nth(1)?;
        rest.split_whitespace().next()?.parse::<i64>().ok()
    }).collect()
}

/// The contact under the character: `(kind, map index)`, or `None` in the air.
pub fn decode_contact(reply: &Reply) -> Option<(u32, u32, u32)> {
    match reply {
        Reply::Bytes { bytes, .. } if bytes.len() >= 0xe4 => {
            let handle = u32::from_le_bytes(bytes[0xe0..0xe4].try_into().unwrap());
            Some((handle, handle & 0xf, (handle >> 4) & 0x3f))
        }
        _ => None,
    }
}

fn contact_command() -> Command { Command::parse("chain contact 16148f0 d0,100,10 228").expect("valid line") }

pub fn contact(install: &Install) -> Result<Option<(u32, u32, u32)>, String> {
    let exchange = probe::request(install, &[contact_command()], Duration::from_secs(3));
    let answers = exchange.outcome.map_err(|reason| format!("{reason}: contato"))?;
    Ok(answers.first().and_then(|a| decode_contact(&a.reply)))
}

fn install(env: &Environment, instance: u8) -> Result<&Install, String> {
    env.installs.iter().find(|i| i.account == instance).ok_or_else(|| format!("instance_missing: conta {instance}"))
}

fn log_size(install: &Install) -> u64 { std::fs::metadata(install.game_dir.join(LOG)).map(|m| m.len()).unwrap_or(0) }

/// Loads `map` and waits until the hook's status has it at estado 5.
pub fn load_and_wait(install: &Install, map: &str, timeout: Duration) -> Result<(Owner, bool, u128), String> {
    let started = Instant::now();
    if let Some(owner) = status(install, Duration::from_secs(5))?.owner(map).filter(|o| o.state == LOADED) {
        return Ok((owner.clone(), true, 0));
    }
    send(install, &[Order::Load(map.to_owned())], Duration::from_secs(5))?;
    let deadline = started + timeout;
    loop {
        crate::control::sleep(Duration::from_millis(500))?;
        if let Some(owner) = status(install, Duration::from_secs(5))?.owner(map).filter(|o| o.state == LOADED) {
            return Ok((owner.clone(), false, started.elapsed().as_millis()));
        }
        if Instant::now() >= deadline {
            return Err(format!("map_not_loaded: o mapa {map} não chegou a estado 5 em {} s", timeout.as_secs()));
        }
    }
}

/// Focuses the streamer on `map` at `at` and waits for a real cell.
pub fn focus_and_wait(install: &Install, map: &str, at: [f32; 3], timeout: Duration) -> Result<i64, String> {
    let from = log_size(install);
    send(install, &[Order::Focus(map.to_owned(), at)], Duration::from_secs(5))?;
    let deadline = Instant::now() + timeout;
    loop {
        crate::control::sleep(Duration::from_millis(300))?;
        let cells = focus_cells(&probe::read_from(&install.game_dir.join(LOG), from).unwrap_or_default(), map);
        if let Some(cell) = cells.iter().find(|c| **c != -1 && **c != -2) { return Ok(*cell); }
        if Instant::now() >= deadline {
            return Err(format!("focus_failed: nenhuma célula válida para {map} em {} s (tentativas: {cells:?})", timeout.as_secs()));
        }
    }
}

/// Waits for two contact reads in a row on the map at `index`.
fn contact_on(install: &Install, index: u32, timeout: Duration) -> Result<Vec<Value>, Vec<Value>> {
    let deadline = Instant::now() + timeout;
    let mut seen = Vec::new();
    let mut streak = 0;
    loop {
        let read = contact(install);
        let on = matches!(read, Ok(Some((_, kind, i))) if (kind == 7 || kind == 1) && i == index);
        seen.push(match &read {
            Ok(Some((handle, kind, i))) => json!({"handle": format!("{handle:x}"), "kind": kind, "index": i}),
            Ok(None) => json!({"inAir": true}),
            Err(e) => json!({"error": e}),
        });
        streak = if on { streak + 1 } else { 0 };
        if streak >= 2 { return Ok(seen); }
        if Instant::now() >= deadline || crate::control::check().is_err() { return Err(seen); }
        let _ = crate::control::sleep(Duration::from_millis(400));
    }
}

/// Load, focus, teleport, confirm the ground, release.
pub fn goto_map(env: &Environment, instance: u8, map: &str, target: [f32; 3]) -> Result<Value, String> {
    let install = install(env, instance)?;
    let map = parse_map(map)?;
    crate::memory::read(install, Duration::from_secs(5))?;
    let mut data = json!({"instance": instance, "map": map, "to": target});
    let (owner, already, load_ms) = load_and_wait(install, &map, Duration::from_secs(30))?;
    data["load"] = json!({"index": owner.index, "alreadyLoaded": already, "ms": load_ms});
    let cell = focus_and_wait(install, &map, target, Duration::from_secs(15))?;
    data["focus"] = json!({"cell": cell});
    let teleport = crate::teleport::teleport(env, instance, target);
    match teleport {
        Ok(t) => data["teleport"] = t,
        Err(e) => { data["teleport"] = json!({"error": e}); crate::output::data(data.clone()); return Err(format!("{e}; o mapa {map} segue carregado e com foco: `backread --instance {instance} unfocus` e `clear`")); }
    }
    match contact_on(install, owner.index, Duration::from_secs(10)) {
        Ok(reads) => data["contact"] = json!(reads),
        Err(reads) => {
            data["contact"] = json!(reads);
            crate::output::data(data.clone());
            crate::output::outcome("inconclusive");
            return Err(format!("inconclusive: o personagem chegou e o contato não mostrou o chão do mapa [{}] em 10 s; mapa e foco seguem pedidos", owner.index));
        }
    }
    send(install, &[Order::Unfocus, Order::Clear], Duration::from_secs(5))?;
    // The old map goes a few seconds later; standing on the new one has to survive that.
    crate::control::sleep(Duration::from_secs(4))?;
    let after = contact_on(install, owner.index, Duration::from_secs(3));
    let position = crate::memory::read(install, Duration::from_secs(3)).map(|c| c.position);
    data["afterRelease"] = json!({"contact": after.as_ref().unwrap_or_else(|e| e), "position": position.as_ref().ok()});
    crate::output::data(data.clone());
    after.map_err(|_| format!("left_map: depois de soltar, o contato não está mais no mapa {map} [{}]", owner.index))?;
    Ok(data)
}

pub fn goto_map_command(env: &Environment, instance: u8, map: &str, to: &str) -> Result<(), String> {
    let target = crate::teleport::parse_point(to)?;
    let data = goto_map(env, instance, map, target)?;
    crate::output::line(format_args!("conta {instance}: no chão do mapa {} [{}], célula {}, carregado em {} ms",
        data["map"].as_str().unwrap_or(""), data["load"]["index"], data["focus"]["cell"], data["load"]["ms"]));
    Ok(())
}

pub fn command(env: &Environment, instance: u8, order: Order) -> Result<(), String> {
    let install = install(env, instance)?;
    let with_status = [order.clone(), Order::Status];
    let orders: &[Order] = if matches!(order, Order::Status) { &with_status[1..] } else { &with_status };
    let status = send(install, orders, Duration::from_secs(5))?.ok_or("no_answer: status sem resposta")?;
    crate::output::line(format_args!("conta {instance}: pedido {} foco {}; {} mapas com estado", status.requested,
        status.focus.as_deref().unwrap_or("?"), status.maps.len()));
    for o in &status.maps {
        crate::output::line(format_args!("  [{}] {} estado {} forçado {}", o.index, o.map, o.state, o.forced));
    }
    crate::output::data(json!({"instance": instance, "order": order.line(), "status": status}));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const STATUS: &str = "13:30:30.101  === backread: 38 mapas; pedido 0a040000 mascara 00000000000000000000000000000000; foco 00000000 ===
    [1] mapa 0a040000 estado 4 forcado 1 quer 1: +10=00000000000000200000000000000000
    [12] mapa 0a1f0000 estado 5 forcado 0 quer 1: +10=02000002000000000000000000000000
";

    #[test]
    fn the_status_block_gives_each_map_its_index_and_state() {
        let status = parse_status(STATUS).unwrap().unwrap();
        assert_eq!(status.owners, 38);
        assert_eq!(status.requested, "0a040000");
        assert_eq!(status.focus.as_deref(), Some("00000000"));
        assert_eq!(status.owner("0a1f0000"), Some(&Owner { index: 12, map: "0a1f0000".into(), state: 5, forced: 0 }));
        assert_eq!(status.owner("0a040000").map(|o| (o.index, o.state, o.forced)), Some((1, 4, 1)));
        // An older build, without the focus and with `partes`.
        let old = "=== backread: 2 mapas; pedido 00000000 mascara 00000000 ===\n    [3] mapa 0a100000 estado 5 forcado 1 partes 00000002\n";
        let status = parse_status(old).unwrap().unwrap();
        assert_eq!(status.focus, None);
        assert_eq!(status.maps[0].index, 3);
    }

    #[test]
    fn orders_are_confirmed_by_their_own_echo() {
        let map = "0a040000".to_owned();
        assert!(Order::Load(map.clone()).echoed("=== pedido: mapa 0a040000 partes todas ==="));
        assert!(!Order::Load(map.clone()).echoed("=== pedido: mapa 0a1f0000 partes todas ==="));
        assert!(Order::Focus(map.clone(), [10.5, 5.9, -16.2]).echoed("=== pedido: foco no mapa 0a040000 em (10.500, 5.900, -16.200) ==="));
        assert!(Order::Clear.echoed("=== pedido: soltar ==="));
        assert_eq!(Order::Focus(map, [10.5, 5.9, -16.25]).line(), "focus 0a040000 10.5 5.9 -16.25");
        assert_eq!(parse_map("0x0A040000"), Ok("0a040000".into()));
        assert!(parse_map("majula").is_err());
    }

    #[test]
    fn a_focus_counts_only_with_a_real_cell() {
        let log = "12:00:00.000  foco no mapa 0a040000 em (10.530, 5.920, -16.250): celula -2 (tentativa 1)
12:00:00.100  foco no mapa 0a1f0000 em (6.186, -18.517, 209.053): celula 201326888 (tentativa 1)
12:00:00.200  foco no mapa 0a040000 em (10.530, 5.920, -16.250): celula 201326888 (tentativa 2)
";
        assert_eq!(focus_cells(log, "0a040000"), vec![-2, 201326888]);
        assert_eq!(focus_cells(log, "0a100000"), Vec::<i64>::new());
    }

    #[test]
    fn the_contact_handle_names_the_map_index() {
        let mut bytes = vec![0u8; 228];
        bytes[0xe0..0xe4].copy_from_slice(&0xc7u32.to_le_bytes());
        assert_eq!(decode_contact(&Reply::Bytes { address: 1, bytes: bytes.clone() }), Some((0xc7, 7, 12)));
        bytes[0xe0..0xe4].copy_from_slice(&0x3c17u32.to_le_bytes());
        assert_eq!(decode_contact(&Reply::Bytes { address: 1, bytes }), Some((0x3c17, 7, 1)));
        assert_eq!(decode_contact(&Reply::ChainUnresolved), None);
    }
}
