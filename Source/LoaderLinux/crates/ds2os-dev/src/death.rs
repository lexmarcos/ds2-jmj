//! The death hook's controls, and deaths on demand.
//!
//! `DS2_DeathInterceptHook` takes orders from `DS2_Death.req` and answers in
//! `DS2_Death.log`. There are no labels: an order is confirmed by the exact
//! line it echoes, found in what the log gained after the order was written.
//! Every launch starts over at `=== ds2os morte: modo observar ===` with every
//! part of the bill on, so a mode set before a relaunch is gone after it — the
//! profile kept in the harness config is how a test gets it back (`game enter`
//! applies it).
//!
//! `kill` zeroes the local character's HP through MemProbe, with the bytes it
//! expects, and passes only on the lines the hook writes for that death.

use std::collections::BTreeMap;
use std::path::Path;
use std::time::{Duration, Instant};

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

use crate::env::{Environment, Install};
use crate::probe::{self, Reply, Where};

const REQUEST: &str = "DS2_Death.req";
const LOG: &str = "DS2_Death.log";
/// The hook knows nothing of it: it keeps the harness's own writers from
/// overwriting each other's orders before the hook reads them.
const LOCK: &str = "DS2_Death.lock";

/// The parts of a death's bill the hook can switch off, as it names them.
pub const FEATURES: [&str; 10] = ["almas", "hollow", "contador", "anel", "mancha_online", "estus", "banner", "copias",
    "fogueira_do_host", "outro_mapa"];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, clap::ValueEnum)]
#[serde(rename_all = "snake_case")]
pub enum Mode {
    /// The game's own death, untouched.
    Observe,
    /// The local player's death is cancelled and nothing is paid.
    Cancel,
    /// The death is paid and the character is back at the bonfire, in the same session.
    Respawn,
}

impl Mode {
    fn verb(self) -> &'static str { match self { Mode::Observe => "observe", Mode::Cancel => "cancel", Mode::Respawn => "respawn" } }
    fn echo(self) -> &'static str {
        match self {
            Mode::Observe => "=== modo: observar ===",
            Mode::Cancel => "=== modo: cancelar a morte do jogador local ===",
            Mode::Respawn => "=== modo: renascer na fogueira, pagando a morte ===",
        }
    }
    fn from_status(name: &str) -> Option<Self> {
        match name { "observar" => Some(Mode::Observe), "cancelar" => Some(Mode::Cancel), "renascer" => Some(Mode::Respawn), _ => None }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Order { Mode(Mode), Feature { name: String, on: bool }, Status }

impl Order {
    pub fn feature(name: &str, on: bool) -> Result<Self, String> {
        if !FEATURES.contains(&name) {
            return Err(format!("unknown_feature: {name}; use uma de {}", FEATURES.join(", ")));
        }
        Ok(Order::Feature { name: name.to_owned(), on })
    }
    fn line(&self) -> String {
        match self {
            Order::Mode(mode) => mode.verb().to_owned(),
            Order::Feature { name, on } => format!("feature {name} {}", if *on { "on" } else { "off" }),
            Order::Status => "status".to_owned(),
        }
    }
}

/// What `status` reports: the mode, the counters since launch and which parts
/// of the bill are on.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Status { pub mode: Mode, pub counters: BTreeMap<String, u64>, pub features: BTreeMap<String, bool> }

/// `=== modo renascer: vistas=0 canceladas=2 ... copias_recusadas=0; almas=1 hollow=1 ... ===`
fn parse_status(text: &str) -> Option<Status> {
    let inner = text.strip_prefix("=== modo ")?.strip_suffix(" ===")?;
    let (mode, rest) = inner.split_once(": ")?;
    let (counters, features) = rest.split_once(';')?;
    fn pairs(part: &str) -> Option<Vec<(&str, &str)>> { part.split_whitespace().map(|kv| kv.split_once('=')).collect() }
    Some(Status {
        mode: Mode::from_status(mode)?,
        counters: pairs(counters)?.into_iter().map(|(k, v)| Some((k.to_owned(), v.parse().ok()?))).collect::<Option<_>>()?,
        features: pairs(features)?.into_iter().map(|(k, v)| Some((k.to_owned(), match v { "1" => true, "0" => false, _ => None? }))).collect::<Option<_>>()?,
    })
}

/// A log line without the hook's `HH:MM:SS.mmm  ` clock.
fn text(line: &str) -> &str {
    let line = line.trim_end_matches(['\n', '\r']);
    match line.split_once("  ") {
        Some((clock, rest)) if clock.len() == 12 && clock.as_bytes()[2] == b':' => rest,
        _ => line,
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum Echo { Done, Status(Status) }

/// Finds each order's echo, in order, in what the log gained.
fn echoes(appended: &str, orders: &[Order]) -> Result<Vec<Option<Echo>>, String> {
    let lines: Vec<&str> = appended.split_inclusive('\n').filter(|l| l.ends_with('\n')).map(text).collect();
    let mut found = Vec::new();
    let mut at = 0;
    for order in orders {
        let wanted = |line: &str| match order {
            Order::Mode(mode) => (line == mode.echo()).then_some(Echo::Done),
            Order::Feature { .. } => (line == format!("=== cobranca: {} ===", order.line())).then_some(Echo::Done),
            Order::Status => parse_status(line).map(Echo::Status),
        };
        let refused = format!("=== nao entendi: {} ===", order.line());
        match lines[at..].iter().enumerate().find_map(|(k, line)| {
            if *line == refused { Some(Err(format!("death_order_refused: o hook não entendeu {:?}", order.line()))) }
            else { wanted(line).map(|echo| Ok((k, echo))) }
        }) {
            Some(Ok((k, echo))) => { at += k + 1; found.push(Some(echo)); }
            Some(Err(e)) => return Err(e),
            None => { found.push(None); break; }
        }
    }
    found.resize(orders.len(), None);
    Ok(found)
}

fn log_size(dir: &Path) -> u64 { std::fs::metadata(dir.join(LOG)).map(|m| m.len()).unwrap_or(0) }

/// Writes the orders and waits for every echo. Passes only on the echo.
pub fn send(install: &Install, orders: &[Order], timeout: Duration) -> Result<Vec<Echo>, String> {
    let deadline = Instant::now() + timeout;
    let dir = install.game_dir.as_path();
    let _lock = loop {
        match crate::control::Lock::acquire(&dir.join(LOCK)) {
            Ok(lock) => break lock,
            Err(e) if !e.starts_with("busy:") => return Err(format!("request_write_failed: {e}")),
            Err(_) if Instant::now() >= deadline => return Err("probe_busy: DS2_Death.lock ocupado".into()),
            Err(_) => crate::control::sleep(Duration::from_millis(50))?,
        }
    };
    // The hook reads the file and then removes it; replacing it in between
    // would lose the new orders unread. So an order waits for the last one to go.
    while dir.join(REQUEST).exists() {
        if Instant::now() >= deadline { return Err("request_not_consumed: um DS2_Death.req anterior não foi lido".into()); }
        crate::control::sleep(Duration::from_millis(100))?;
    }
    let from = log_size(dir);
    let body: String = orders.iter().map(|o| o.line() + "\n").collect();
    let temporary = dir.join("DS2_Death.req.tmp");
    std::fs::write(&temporary, &body).and_then(|_| std::fs::rename(&temporary, dir.join(REQUEST)))
        .map_err(|e| format!("request_write_failed: {e}"))?;
    let result = loop {
        crate::control::check()?;
        let found = echoes(&probe_read_from(dir, from), orders)?;
        if found.iter().all(Option::is_some) { break Ok(found.into_iter().map(Option::unwrap).collect()); }
        if Instant::now() >= deadline {
            break Err(if dir.join(REQUEST).exists() {
                "request_not_consumed: o hook de morte não leu o pedido (jogo iniciando, ou DLL sem o hook)".to_owned()
            } else { format!("no_answer: {} de {} ordens confirmadas", found.iter().filter(|e| e.is_some()).count(), orders.len()) });
        }
        crate::control::sleep(Duration::from_millis(100))?;
    };
    crate::output::event("death_orders", json!({"instance": install.account, "orders": orders.iter().map(Order::line).collect::<Vec<_>>(),
        "ok": result.is_ok(), "error": result.as_ref().err(),
        "status": result.as_ref().ok().and_then(|echoes: &Vec<Echo>| echoes.iter().find_map(|x| match x { Echo::Status(s) => Some(s.clone()), _ => None }))}));
    result
}

fn probe_read_from(dir: &Path, from: u64) -> String { probe::read_from(&dir.join(LOG), from).unwrap_or_default() }

pub fn status(install: &Install, timeout: Duration) -> Result<Status, String> {
    match send(install, &[Order::Status], timeout)?.pop() {
        Some(Echo::Status(status)) => Ok(status),
        _ => Err("no_answer: status sem resposta".into()),
    }
}

/// The mode and bill a test wants every launch to start with.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Profile { pub mode: Option<Mode>, #[serde(default)] pub features: BTreeMap<String, bool> }

impl Profile {
    pub fn orders(&self) -> Vec<Order> {
        self.mode.map(Order::Mode).into_iter()
            .chain(self.features.iter().map(|(name, on)| Order::Feature { name: name.clone(), on: *on })).collect()
    }
}

/// Applies the saved profile to a game that is running this boot's hook.
pub fn apply_profile(install: &Install, profile: &Profile, timeout: Duration) -> Result<(), String> {
    let orders = profile.orders();
    if orders.is_empty() { return Ok(()); }
    let installed = crate::observe::hooks(&install.game_dir)
        .and_then(|receipt| receipt.pointer("/hooks/DS2 Death Intercept").and_then(Value::as_bool));
    if installed != Some(true) {
        return Err("death_profile_unapplied: o recibo deste boot não mostra DS2 Death Intercept instalado".into());
    }
    send(install, &orders, timeout).map(|_| ())
}

/// What a death wrote, by the mode it happened in.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Signal { pub complete: bool, pub lines: Vec<String> }

/// The lines that prove a death happened the way the mode says it should.
///
/// - respawn: one `custos da morte (...)` and then `renascer concluido`. The
///   `morte CANCELADA` between them is how the hook stops the game's own death,
///   not a failure.
/// - cancel: `morte CANCELADA`, and no bill.
/// - observe: `morte vista`.
fn death_signal(appended: &str, mode: Mode) -> Signal {
    let lines: Vec<&str> = appended.split_inclusive('\n').filter(|l| l.ends_with('\n')).map(text).collect();
    let relevant: Vec<String> = lines.iter().filter(|l| ["custos da morte", "renascer concluido", "morte CANCELADA", "morte vista",
        "queda concluido", "renascer:", "mapa "].iter().any(|k| l.starts_with(k))).map(|l| l.to_string()).collect();
    let position = |prefix: &str| lines.iter().position(|l| l.starts_with(prefix));
    let complete = match mode {
        Mode::Respawn => match (position("custos da morte ("), lines.iter().rposition(|l| l.starts_with("renascer concluido em "))) {
            (Some(cost), Some(done)) => cost < done && lines.iter().filter(|l| l.starts_with("custos da morte (")).count() == 1,
            _ => false,
        },
        Mode::Cancel => position("morte CANCELADA").is_some() && position("custos da morte").is_none(),
        Mode::Observe => position("morte vista").is_some(),
    };
    Signal { complete, lines: relevant }
}

/// Kills the local character of `instance` and waits for the hook to say so.
///
/// In `observe` mode that is the game's own death — souls on the ground, a
/// load, and in a session the end of it — so it needs `real_death`.
pub fn kill(env: &Environment, instance: u8, real_death: bool, timeout: Duration) -> Result<Value, String> {
    let install = env.installs.iter().find(|i| i.account == instance).ok_or_else(|| format!("instance_missing: conta {instance}"))?;
    if crate::observe::processes(env, instance).is_empty() { return Err(format!("instance_stopped: conta {instance}")); }
    let located = probe::locate(install, Duration::from_secs(3));
    if located.state != Where::World {
        return Err(format!("not_in_world: conta {instance} está em {} ({})", located.state, located.reason));
    }
    let mode = status(install, Duration::from_secs(5))?.mode;
    if mode == Mode::Observe && !real_death {
        return Err("death_mode_observe: o hook está em observe, e a morte seria a do jogo; use `death mode respawn` ou --real-death".into());
    }
    let before = crate::memory::read(install, Duration::from_secs(5))?;
    if before.hp <= 0 { return Err(format!("already_dead: hp {}", before.hp)); }

    let others: Vec<&Install> = env.installs.iter().filter(|i| i.account != instance).collect();
    let from = log_size(&install.game_dir);
    let others_from: Vec<u64> = others.iter().map(|i| log_size(&i.game_dir)).collect();
    let address = u64::from_str_radix(before.address.trim_start_matches("0x"), 16).map_err(|e| e.to_string())?;
    let hex = |v: i32| v.to_le_bytes().iter().map(|b| format!("{b:02x}")).collect::<String>();
    let poke = probe::Command::parse(&format!("pokeabs hp {:x} 00000000 {}", address + crate::memory::HP, hex(before.hp)))?;
    let exchange = probe::request(install, &[poke], Duration::from_secs(5));
    let answer = exchange.outcome.map_err(|reason| format!("{reason}: o HP não foi escrito"))?.remove(0);
    let mut data = json!({"instance": instance, "mode": mode, "before": before, "poke": answer.json()});
    match answer.reply {
        Reply::Poked { wrote: true, .. } => {}
        _ => { crate::output::data(data); return Err("poke_not_written: o injector não escreveu o HP; nada morreu".into()); }
    }

    let deadline = Instant::now() + timeout;
    let signal = loop {
        crate::control::check()?;
        let signal = death_signal(&probe_read_from(&install.game_dir, from), mode);
        if signal.complete || Instant::now() >= deadline { break signal; }
        crate::control::sleep(Duration::from_millis(150))?;
    };
    // A copy of this character dying on another machine is refused there.
    let other_side: Vec<Value> = others.iter().zip(&others_from).map(|(other, from)| {
        let refused: Vec<String> = probe_read_from(&other.game_dir, *from).split_inclusive('\n').map(text)
            .filter(|l| l.starts_with("morte da copia RECUSADA")).map(str::to_owned).collect();
        json!({"instance": other.account, "refused": refused.len(), "lines": refused})
    }).collect();
    data["deathLines"] = json!(signal.lines);
    data["otherSide"] = json!(other_side);
    if signal.complete && mode != Mode::Observe {
        // Give the recovery its frame, then read the character it left.
        let _ = crate::control::sleep(Duration::from_millis(300));
        data["after"] = match crate::memory::read(install, Duration::from_secs(5)) { Ok(c) => json!(c), Err(e) => json!({"error": e}) };
    }
    crate::output::data(data.clone());
    if !signal.complete {
        crate::output::outcome("inconclusive");
        return Err(format!("inconclusive: o HP foi zerado e o DS2_Death.log não mostrou a morte completa em modo {} em {}s",
            mode.verb(), timeout.as_secs()));
    }
    Ok(data)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Lines copied from DS2_Death.log on 14/09.
    const RESPAWN: &str = "13:34:18.348  custos da morte (papel 0, convidado 0): almas 0 -> 0 (registradas: 0 almas para a mancha, 0 perdidas da anterior); hollow aplicado: nivel 0 -> 1, hp maximo 915 -> 869; deste jogador: mortes 55 -> 56, anel conferido; manchas: 1 antiga(s) removida(s), nova criada, agora 1 no mundo; mancha online desligado; estus recarregado
13:34:18.349  morte CANCELADA #1 (seguida 1) hp=0 -> 915 matador=00000000 flags=00000000 causa=10 +0x4c0=0000000000000000 bruto=0000000000000000000000000a0000000000
13:34:19.032  renascer concluido em 1 quadros: +0x4c0 0000000000000000 -> 0000000000000000, camera de queda nao estava ligada, hp 915 -> 869
";
    const REFUSED: &str = "13:34:18.381  morte da copia RECUSADA #1 (seguida 1) controlador 00007FFFEB897D50 personagem 00007FFFEB7B9E60 tipo 2 papel 0 hp=0 -> 869 matador=00000000 flags=00000000 causa=10 +0x4c0=0000000000300000 bruto=0000000000000000000000000a0000000000\n";
    const SEEN: &str = "13:38:40.535  morte vista hp=0 max=854 matador=00000000 flags=00000000 causa=10 +0x4c0=0000000000000000 bruto=0000000000000000000000000a0000000000\n";

    #[test]
    fn a_respawn_is_the_bill_and_then_the_recovery() {
        assert!(death_signal(RESPAWN, Mode::Respawn).complete);
        assert_eq!(death_signal(RESPAWN, Mode::Respawn).lines.len(), 3);
        // The bill without the recovery has not finished.
        let (paid, _) = RESPAWN.split_at(RESPAWN.find("13:34:19.032").unwrap());
        assert!(!death_signal(paid, Mode::Respawn).complete);
        // Two bills for one kill is the double death the fall bug made.
        assert!(!death_signal(&format!("{RESPAWN}{RESPAWN}"), Mode::Respawn).complete);
        // In cancel mode nothing may be paid.
        assert!(!death_signal(RESPAWN, Mode::Cancel).complete);
        assert!(death_signal(&RESPAWN.lines().nth(1).map(|l| format!("{l}\n")).unwrap(), Mode::Cancel).complete);
        assert!(death_signal(SEEN, Mode::Observe).complete);
        // The other machine's refusal is not this machine's death.
        for mode in [Mode::Respawn, Mode::Cancel, Mode::Observe] { assert!(!death_signal(REFUSED, mode).complete); }
        // A line still being written does not count.
        assert!(!death_signal(SEEN.trim_end(), Mode::Observe).complete);
    }

    #[test]
    fn orders_pass_only_on_their_echo() {
        let status = "13:40:00.000  === modo renascer: vistas=0 canceladas=2 renascimentos=1 recuperacoes=1 recuperacoes_falhas=0 sem_+0x759=0 instantaneas=0 chamadas_slot_+0x10=0 outros_controladores=3 copias_recusadas=2; almas=1 hollow=1 contador=1 anel=1 mancha_online=1 estus=1 banner=1 copias=0 fogueira_do_host=1 outro_mapa=1 ===\n";
        let orders = vec![Order::Mode(Mode::Respawn), Order::feature("copias", false).unwrap(), Order::Status];
        let log = format!("13:25:52.669  === modo: renascer na fogueira, pagando a morte ===\n{SEEN}13:38:36.359  === cobranca: feature copias off ===\n{status}");
        let found = echoes(&log, &orders).unwrap();
        assert_eq!(found[0], Some(Echo::Done));
        let Some(Echo::Status(parsed)) = &found[2] else { panic!("{found:?}") };
        assert_eq!(parsed.mode, Mode::Respawn);
        assert_eq!(parsed.counters["copias_recusadas"], 2);
        assert_eq!(parsed.counters["sem_+0x759"], 0);
        assert_eq!(parsed.features["copias"], false);
        assert_eq!(parsed.features.len(), FEATURES.len());
        // Out of order, or missing, is not confirmed.
        assert_eq!(echoes(&log, &[Order::Status, Order::Mode(Mode::Respawn)]).unwrap()[1], None);
        assert_eq!(echoes("13:15:15.112  === ds2os morte: modo observar ===\n", &[Order::Mode(Mode::Observe)]).unwrap(), vec![None]);
        // A refusal is a failure, never a pass.
        let refused = "13:00:00.000  === nao entendi: feature copias off ===\n";
        assert!(echoes(refused, &orders[1..2]).unwrap_err().starts_with("death_order_refused:"));
        assert!(Order::feature("copia", false).is_err());
    }

    #[test]
    fn a_profile_is_mode_first_then_each_feature() {
        let profile = Profile { mode: Some(Mode::Respawn), features: [("copias".to_owned(), false)].into() };
        assert_eq!(profile.orders().iter().map(Order::line).collect::<Vec<_>>(), vec!["respawn", "feature copias off"]);
        assert!(Profile::default().orders().is_empty());
    }
}
