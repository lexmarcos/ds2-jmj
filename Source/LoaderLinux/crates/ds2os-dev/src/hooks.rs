//! Puts the injector's request-driven hooks back where a fresh arrival has them.
//!
//! A test that blocks a session reason, holds a map loaded, arms a breakpoint or
//! switches the death bill leaves that in the running game, and the next test
//! inherits it without a word. `hooks reset` undoes each one and passes only on
//! the echo each hook writes for the reset, never on a request that was merely
//! written.
//!
//! What it cannot undo is said, not hidden: `DS2_Backread` has no verb that
//! drops a `keep` (only its deadline does), so maps still forced after `clear`
//! come back as `keepsRemain`. In a live session those are the remote copy's
//! own keeps and are expected.

use std::collections::BTreeMap;
use std::time::Duration;

use serde::Serialize;
use serde_json::{json, Value};

use crate::death::{self, Echo, Mode, Order};
use crate::env::Install;
use crate::hook_request;

const TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HookReset {
    pub hook: &'static str,
    /// `reset`, `skipped` (not in this boot's receipt) or `failed`.
    pub outcome: &'static str,
    pub error: Option<String>,
    pub data: Value,
    pub warnings: Vec<String>,
}

/// Finds `wanted` in order among the complete lines, each after the previous.
fn in_order<'a>(lines: &[&'a str], wanted: &[&dyn Fn(&str) -> bool]) -> Option<Vec<&'a str>> {
    let mut at = 0;
    let mut found = Vec::new();
    for want in wanted {
        let k = lines[at..].iter().position(|l| want(l))?;
        found.push(lines[at + k]);
        at += k + 1;
    }
    Some(found)
}

/// `=== recusando a mascara 00000000, papel -1 ===`
fn parse_session(appended: &str) -> Result<Option<(u32, i32)>, String> {
    let lines = hook_request::lines(appended);
    let Some(found) = in_order(&lines, &[
        &|l| l == "=== nada mais e recusado ===",
        &|l| l == "=== recusa vale para qualquer papel ===",
        &|l| l.starts_with("=== recusando a mascara "),
    ]) else { return Ok(None) };
    let status = found[2];
    let inner = status.trim_start_matches("=== recusando a mascara ").trim_end_matches(" ===");
    let (mask, role) = inner.split_once(", papel ").ok_or_else(|| format!("malformed_answer: {status:?}"))?;
    Ok(Some((u32::from_str_radix(mask, 16).map_err(|_| format!("malformed_answer: {status:?}"))?,
        role.parse().map_err(|_| format!("malformed_answer: {status:?}"))?)))
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Backread { pub owners: u32, pub requested: String, pub focus: String, pub forced: Vec<String> }

/// ```text
/// 13:30:30.100  === pedido: sem foco ===
/// 13:30:30.100  === pedido: soltar ===
/// 13:30:30.101  === backread: 2 mapas; pedido 00000000 mascara ...; foco 00000000 ===
///     [1] mapa 0a1f0000 estado 2 forcado 1 quer 1: +10=...
/// ```
fn parse_backread(appended: &str) -> Result<Option<Backread>, String> {
    let lines = hook_request::lines(appended);
    let Some(found) = in_order(&lines, &[
        &|l| l == "=== pedido: sem foco ===",
        &|l| l == "=== pedido: soltar ===",
        &|l| l.starts_with("=== backread: "),
    ]) else { return Ok(None) };
    let header = found[2];
    let malformed = || format!("malformed_answer: {header:?}");
    let words: Vec<&str> = header.split_whitespace().collect();
    let after = |key: &str| words.iter().position(|w| *w == key).and_then(|k| words.get(k + 1)).map(|w| w.trim_end_matches(';'));
    let owners = words.get(2).and_then(|w| w.parse().ok()).ok_or_else(malformed)?;
    let requested = after("pedido").ok_or_else(malformed)?.to_owned();
    let focus = after("foco").ok_or_else(malformed)?.to_owned();
    // The owner lines come in the same append as the header.
    let start = lines.iter().position(|l| *l == header).unwrap_or(0) + 1;
    let forced = lines[start..].iter().map(|l| l.trim_start()).take_while(|l| l.starts_with('['))
        .filter_map(|l| {
            let w: Vec<&str> = l.split_whitespace().collect();
            let map = w.iter().position(|x| *x == "mapa").and_then(|k| w.get(k + 1))?;
            let forced = w.iter().position(|x| *x == "forcado").and_then(|k| w.get(k + 1))?;
            (*forced != "0").then(|| (*map).to_owned())
        }).collect();
    Ok(Some(Backread { owners, requested, focus, forced }))
}

/// `=== limpo ===` then `=== 0 armados, 3 ja alcancados ===`. `wpclear` writes
/// nothing when no watch is armed, so it is sent and not waited on.
fn parse_trace(appended: &str) -> Result<Option<(u64, u64)>, String> {
    let lines = hook_request::lines(appended);
    let Some(found) = in_order(&lines, &[&|l| l == "=== limpo ===", &|l| l.ends_with(" ja alcancados ===")]) else { return Ok(None) };
    let words: Vec<&str> = found[1].split_whitespace().collect();
    match (words.get(1).and_then(|w| w.parse().ok()), words.get(3).and_then(|w| w.parse().ok())) {
        (Some(armed), Some(reached)) => Ok(Some((armed, reached))),
        _ => Err(format!("malformed_answer: {:?}", found[1])),
    }
}

/// The bill a launch starts with: everything on but the online bloodstain.
pub fn death_boot_features() -> BTreeMap<String, bool> {
    death::FEATURES.iter().map(|f| (f.to_string(), *f != "mancha_online")).collect()
}

/// Observe mode and the boot bill, then the saved profile on top: the state a
/// `game enter` leaves.
fn death_orders(profile: Option<&death::Profile>) -> (Vec<Order>, Mode, BTreeMap<String, bool>) {
    let mut features = death_boot_features();
    let mut orders: Vec<Order> = vec![Order::Mode(Mode::Observe)];
    orders.extend(features.iter().map(|(name, on)| Order::Feature { name: name.clone(), on: *on }));
    let mut mode = Mode::Observe;
    if let Some(profile) = profile {
        orders.extend(profile.orders());
        if let Some(m) = profile.mode { mode = m; }
        for (name, on) in &profile.features { features.insert(name.clone(), *on); }
    }
    orders.push(Order::Status);
    (orders, mode, features)
}

fn installed(receipt: &Option<Value>, hook: &str) -> bool {
    receipt.as_ref().and_then(|r| r.get("hooks")).and_then(|h| h.get(hook)).and_then(Value::as_bool) == Some(true)
}

fn one(hook: &'static str, receipt: &Option<Value>, run: impl FnOnce() -> Result<(Value, Vec<String>), String>) -> HookReset {
    if !installed(receipt, hook) {
        return HookReset { hook, outcome: "skipped", error: None, data: json!(null), warnings: vec![] };
    }
    match run() {
        Ok((data, warnings)) => HookReset { hook, outcome: "reset", error: None, data, warnings },
        Err(e) => HookReset { hook, outcome: "failed", error: Some(e), data: json!(null), warnings: vec![] },
    }
}

/// Resets every hook this boot's receipt shows installed, one request file each.
pub fn reset(install: &Install) -> Result<Vec<HookReset>, String> {
    let receipt = crate::observe::hooks(&install.game_dir);
    if receipt.is_none() {
        return Err("receipt_missing: sem recibo deste boot em DS2_Harness.json; não há como saber quais hooks existem".into());
    }
    let dir = &install.game_dir;
    let profile = crate::settings::HarnessConfig::load().death_profile;
    let results = vec![
        one("DS2 Seamless Session", &receipt, || {
            let (mask, role) = hook_request::exchange(dir, "DS2_Session", "clear\nrole any\nstatus\n", TIMEOUT, parse_session)?;
            if mask != 0 || role != -1 {
                return Err(format!("not_reset: o hook respondeu mascara {mask:08x}, papel {role}"));
            }
            Ok((json!({"mask": format!("{mask:08x}"), "role": role}), vec![]))
        }),
        one("DS2 Backread", &receipt, || {
            let state = hook_request::exchange(dir, "DS2_Backread", "unfocus\nclear\nstatus\n", TIMEOUT, parse_backread)?;
            if state.requested != "00000000" || state.focus != "00000000" {
                return Err(format!("not_reset: o hook respondeu pedido {}, foco {}", state.requested, state.focus));
            }
            let warnings = if state.forced.is_empty() { vec![] } else {
                vec![format!("keeps_remain: {} mapa(s) ainda forçados ({}); não há verbo que solte um keep antes do prazo — numa sessão são os da cópia remota",
                    state.forced.len(), state.forced.join(", "))]
            };
            Ok((json!(state), warnings))
        }),
        one("DS2 Trace", &receipt, || {
            let (armed, reached) = hook_request::exchange(dir, "DS2_Trace", "wpclear\nclear\nreport\n", TIMEOUT, parse_trace)?;
            if armed != 0 { return Err(format!("not_reset: {armed} breakpoint(s) ainda armados")); }
            Ok((json!({"armed": armed, "reached": reached}), vec![]))
        }),
        one("DS2 Death Intercept", &receipt, || {
            let (orders, mode, features) = death_orders(profile.as_ref());
            let status = match death::send(install, &orders, TIMEOUT)?.pop() {
                Some(Echo::Status(status)) => status,
                _ => return Err("no_answer: status sem resposta".into()),
            };
            if status.mode != mode || status.features != features {
                return Err(format!("not_reset: o hook respondeu modo {:?} e {:?}", status.mode, status.features));
            }
            Ok((json!({"mode": status.mode, "features": status.features, "profileApplied": profile.is_some()}), vec![]))
        }),
    ];
    crate::output::event("hooks_reset", json!({"instance": install.account, "results": results}));
    Ok(results)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_session_hook_confirms_nothing_is_refused() {
        let log = "=== nada mais e recusado ===\n=== recusa vale para qualquer papel ===\n=== recusando a mascara 00000000, papel -1 ===\n";
        assert_eq!(parse_session(log), Ok(Some((0, -1))));
        assert_eq!(parse_session("=== nada mais e recusado ===\n"), Ok(None));
        // A status from before the reset does not count.
        let stale = "=== recusando a mascara 00000004, papel 2 ===\n=== nada mais e recusado ===\n=== recusa vale para qualquer papel ===\n";
        assert_eq!(parse_session(stale), Ok(None));
    }

    #[test]
    fn the_backread_status_lists_maps_still_forced() {
        let log = "13:30:30.100  === pedido: sem foco ===\n13:30:30.100  === pedido: soltar ===\n\
13:30:30.101  === backread: 2 mapas; pedido 00000000 mascara todas; foco 00000000 ===\n\
    [0] mapa 0a120000 estado 2 forcado 0 quer 1: +10=todas\n    [1] mapa 0a1f0000 estado 2 forcado 1 quer 1: +10=todas\n";
        let state = parse_backread(log).unwrap().unwrap();
        assert_eq!(state.owners, 2);
        assert_eq!(state.requested, "00000000");
        assert_eq!(state.focus, "00000000");
        assert_eq!(state.forced, vec!["0a1f0000".to_owned()]);
    }

    #[test]
    fn the_trace_report_counts_what_is_still_armed() {
        assert_eq!(parse_trace("\n=== limpo ===\n=== 0 armados, 3 ja alcancados ===\n"), Ok(Some((0, 3))));
        assert_eq!(parse_trace("=== 0 armados, 3 ja alcancados ===\n"), Ok(None));
    }

    #[test]
    fn the_death_reset_is_the_boot_bill_with_the_profile_on_top() {
        let (orders, mode, features) = death_orders(None);
        assert_eq!(orders.first(), Some(&Order::Mode(Mode::Observe)));
        assert_eq!(orders.last(), Some(&Order::Status));
        assert_eq!(mode, Mode::Observe);
        assert_eq!(features.get("mancha_online"), Some(&false));
        assert_eq!(features.values().filter(|on| **on).count(), 9);
        let profile = death::Profile { mode: Some(Mode::Respawn), features: [("copias".to_owned(), false)].into() };
        let (_, mode, features) = death_orders(Some(&profile));
        assert_eq!(mode, Mode::Respawn);
        assert_eq!(features.get("copias"), Some(&false));
    }
}
