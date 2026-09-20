//! Moving the local character, and the bonfires it could be moved to.
//!
//! The teleport is the one the death hook uses (`TeleportLocal` in
//! `DS2_DeathInterceptHook.cpp`), done from outside: the game's own copies of
//! the position, the velocity tracker, the Havok body, which is the one that
//! counts, and the fall controller's last ground position. That last one is
//! not optional: a landing is measured from it, and a teleport 24.5 m down
//! without it was a fall death (measured 14/09).
//!
//! Everything is read in one round trip, both vtables are checked, and every
//! write carries the bytes it read, so a heap address reused in between, or a
//! character that moved, is refused by the injector instead of written over.
//! It passes only when all thirteen were written, the character stands within
//! 1.5 m of the target across and 1 m up or down, and the death hook wrote no
//! death for three seconds. Not closer: the physics pushes a character out of
//! whatever it was put inside, and a bonfire's own spawn point is inside the
//! bonfire. Measured 14/09, Heide's cathedral bonfire: 13/13 writes and the
//! character settled 0.84 m away, as the hook's respawn settles up to 1.7 m.

use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::{json, Value};

use crate::env::{Environment, Install};
use crate::probe::{self, Command, Reply};

const CHR_VFTABLE: u64 = 0x1410e4bb8;
const BODY_VFTABLE: u64 = 0x141126578;
const BODY_ABOVE_FEET: f32 = 0.05;
const SPAWN_BEHIND: f32 = 1.1;
pub const ARRIVAL_ACROSS: f32 = 1.5;
pub const ARRIVAL_UPDOWN: f32 = 1.0;
const DEATH_QUIET: Duration = Duration::from_secs(3);

/// The objects a teleport writes, each read whole enough for its fields.
const READS: [&str; 5] = [
    "chain chr 16148f0 d0 376",
    "chain motion 16148f0 d0,f8 96",
    "chain phys 16148f0 d0,100 464",
    "chain body 16148f0 d0,100,320,20 624",
    "chain fall 16148f0 d0,e0,b0 48",
];

/// (label, object, offset, what goes there), in the hook's order, fall last.
#[derive(Clone, Copy)]
enum Value3 { Feet, Centre, Still }
const WRITES: [(&str, usize, usize, Value3); 13] = [
    ("a", 0, 0x90, Value3::Feet), ("b", 0, 0xa0, Value3::Feet), ("c", 2, 0x80, Value3::Feet),
    ("d", 1, 0x50, Value3::Feet), ("e", 2, 0x60, Value3::Still), ("f", 2, 0x70, Value3::Still),
    ("g", 3, 0x250, Value3::Centre), ("h", 3, 0x260, Value3::Centre), ("i", 3, 0x1b0, Value3::Centre),
    ("j", 3, 0x1c0, Value3::Centre), ("k", 3, 0x1a0, Value3::Centre), ("l", 2, 0x1c0, Value3::Centre),
    ("m", 4, 0x20, Value3::Feet),
];

fn hex(bytes: &[u8]) -> String { bytes.iter().map(|b| format!("{b:02x}")).collect() }
fn floats(values: &[f32]) -> Vec<u8> { values.iter().flat_map(|v| v.to_le_bytes()).collect() }

pub fn read_commands() -> Vec<Command> {
    READS.iter().map(|l| Command::parse(l).expect("valid line")).collect()
}

/// The thirteen writes, each with the bytes the read found there, after both vtables check out.
pub fn plan(replies: &[&Reply], target: [f32; 3]) -> Result<Vec<Command>, String> {
    let names = ["personagem", "motion", "física", "corpo Havok", "controle de queda"];
    let mut objects = Vec::new();
    for (reply, name) in replies.iter().zip(names) {
        match reply {
            Reply::Bytes { address, bytes } => objects.push((*address, bytes.as_slice())),
            Reply::ChainUnresolved | Reply::Unreadable { .. } =>
                return Err(format!("no_character: {name} não resolveu; sem personagem local (título ou carregamento)")),
            other => return Err(format!("malformed_answer: {name}: {other:?}")),
        }
    }
    if objects.len() != READS.len() { return Err("malformed_answer: faltam leituras".into()); }
    let vtable = |i: usize| u64::from_le_bytes(objects[i].1[0..8].try_into().unwrap());
    if vtable(0) != CHR_VFTABLE || vtable(3) != BODY_VFTABLE {
        return Err(format!("vtable_mismatch: personagem {:#x}, corpo {:#x}; nada foi escrito", vtable(0), vtable(3)));
    }
    let [x, y, z] = target;
    WRITES.iter().map(|(label, object, offset, value)| {
        let (address, bytes) = objects[*object];
        let data = match value {
            Value3::Feet => floats(&[x, y, z]),
            Value3::Centre => floats(&[x, y + BODY_ABOVE_FEET, z]),
            Value3::Still => vec![0u8; 16],
        };
        let expected = bytes.get(*offset..offset + data.len())
            .ok_or_else(|| format!("malformed_answer: leitura curta para +{offset:#x}"))?;
        Command::parse(&format!("pokeabs {label} {:x} {} {}", address + *offset as u64, hex(&data), hex(expected)))
    }).collect()
}

/// All thirteen written, or which were not.
pub fn judge(replies: &[(&str, &Reply)]) -> Result<usize, String> {
    let mut refused = Vec::new();
    for (name, reply) in replies {
        match reply {
            Reply::Poked { wrote: true, .. } => {}
            Reply::PokeRefused { .. } => refused.push(format!("{name} recusado (os bytes mudaram)")),
            other => refused.push(format!("{name}: {other:?}")),
        }
    }
    if refused.is_empty() { return Ok(replies.len()); }
    Err(format!("teleport_refused: {} de {} escritas não entraram: {}; o personagem precisa estar parado",
        refused.len(), replies.len(), refused.join(", ")))
}

/// (across, up or down) from `b` to `a`.
fn offset(a: [f32; 3], b: [f32; 3]) -> (f32, f32) {
    (((a[0] - b[0]).powi(2) + (a[2] - b[2]).powi(2)).sqrt(), (a[1] - b[1]).abs())
}

fn arrived_at(a: [f32; 3], target: [f32; 3]) -> bool {
    let (across, updown) = offset(a, target);
    across < ARRIVAL_ACROSS && updown < ARRIVAL_UPDOWN
}

fn install(env: &Environment, instance: u8) -> Result<&Install, String> {
    env.installs.iter().find(|i| i.account == instance).ok_or_else(|| format!("instance_missing: conta {instance}"))
}

fn death_log_size(install: &Install) -> u64 {
    std::fs::metadata(install.game_dir.join("DS2_Death.log")).map(|m| m.len()).unwrap_or(0)
}

/// Death lines the hook wrote since `from`.
fn deaths_since(install: &Install, from: u64) -> Vec<String> {
    probe::read_from(&install.game_dir.join("DS2_Death.log"), from).unwrap_or_default().lines()
        .map(crate::hook_request::text)
        .filter(|l| matches!(crate::timeline::classify("death", l), Some("death_cost" | "death_cancelled" | "death_seen")))
        .map(str::to_owned).collect()
}

/// Teleports and waits for the evidence. Emits no result data of its own, so
/// `goto-map` can use it as a step.
pub fn teleport(env: &Environment, instance: u8, target: [f32; 3]) -> Result<Value, String> {
    let install = install(env, instance)?;
    if !target.iter().all(|v| v.is_finite()) { return Err("invalid_target: coordenadas finitas".into()); }
    let exchange = probe::request(install, &read_commands(), Duration::from_secs(5));
    let answers = exchange.outcome.map_err(|reason| format!("{reason}: leitura antes do teleporte"))?;
    let replies: Vec<&Reply> = answers.iter().map(|a| &a.reply).collect();
    let from = match &replies[0] {
        Reply::Bytes { bytes, .. } => [0x90usize, 0x94, 0x98].map(|at| f32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())),
        _ => [f32::NAN; 3],
    };
    let writes = plan(&replies, target)?;
    let deaths_from = death_log_size(install);
    let exchange = probe::request(install, &writes, Duration::from_secs(5));
    let answers = exchange.outcome.map_err(|reason| format!("{reason}: as escritas não voltaram; o estado do personagem é desconhecido"))?;
    let mut data = json!({"instance": instance, "from": from, "to": target, "writes": answers.iter().map(|a| a.json()).collect::<Vec<_>>()});
    let written = judge(&answers.iter().map(|a| (a.name.as_str(), &a.reply)).collect::<Vec<_>>())
        .inspect_err(|_| crate::output::data(data.clone()))?;
    data["written"] = json!(written);

    // Where it stands now, from the character itself.
    // It settles over a few frames, so the verdict is on where it stands after the quiet period.
    let quiet_until = Instant::now() + DEATH_QUIET;
    let first = crate::memory::read(install, Duration::from_secs(3)).map(|c| c.position);
    while Instant::now() < quiet_until { crate::control::sleep(Duration::from_millis(250))?; }
    let settled = crate::memory::read(install, Duration::from_secs(3)).map(|c| c.position);
    let deaths = deaths_since(install, deaths_from);
    data["firstRead"] = json!(first.as_ref().ok());
    data["after"] = json!(settled.as_ref().ok());
    if let Ok(position) = &settled {
        let (across, updown) = offset(*position, target);
        data["across"] = json!(across);
        data["upDown"] = json!(updown);
    }
    data["deathLines"] = json!(deaths);
    let arrived = settled.as_ref().is_ok_and(|p| arrived_at(*p, target));
    if !deaths.is_empty() {
        crate::output::data(data.clone());
        return Err(format!("died_after_teleport: o hook de morte escreveu {}", deaths.join(" | ")));
    }
    if !arrived {
        crate::output::data(data.clone());
        return Err(format!("not_arrived: as 13 escritas entraram e o personagem assentou fora de {ARRIVAL_ACROSS} m na horizontal e {ARRIVAL_UPDOWN} m na vertical do destino"));
    }
    Ok(data)
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BonfireNode { pub id: String, pub map: String, pub kind: u8, pub spawn: [f32; 3] }

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Bonfires { pub record: Option<crate::memory::Bonfire>, pub nodes: Vec<BonfireNode>, pub truncated: bool }

const MAX_NODES: usize = 16;

/// One request: the record, and for each of the first nodes of its list the
/// object, the map holder and the id. The list is a path from the context, so
/// nodes past the end come back unresolved instead of needing a walk.
pub fn bonfire_commands() -> Vec<Command> {
    let mut lines = vec!["chain record 16148f0 70 368".to_owned()];
    for k in 0..MAX_NODES {
        let node = format!("70,58,8{}", ",60".repeat(k));
        lines.push(format!("chain o{k} 16148f0 {node},8 192"));
        lines.push(format!("chain h{k} 16148f0 {node},8,28 12"));
        lines.push(format!("chain i{k} 16148f0 {node},8,b8,20,e0 4"));
    }
    lines.iter().map(|l| Command::parse(l).expect("valid line")).collect()
}

pub fn decode_bonfires(replies: &[&Reply]) -> Result<Bonfires, String> {
    let record = match replies.first() {
        Some(Reply::Bytes { bytes, .. }) if bytes.len() >= 0x170 => Some(crate::memory::Bonfire {
            map: u32::from_le_bytes(bytes[0x164..0x168].try_into().unwrap()),
            id: u32::from_le_bytes(bytes[0x16c..0x170].try_into().unwrap()),
        }),
        Some(Reply::ChainUnresolved | Reply::Unreadable { .. }) => return Err("no_character: registro de fogueira não resolveu (título ou carregamento)".into()),
        _ => None,
    };
    let f32_at = |b: &[u8], at: usize| f32::from_le_bytes(b[at..at + 4].try_into().unwrap());
    let mut nodes = Vec::new();
    let mut truncated = true;
    for k in 0..MAX_NODES {
        let (Some(object), Some(holder), Some(id)) = (replies.get(1 + 3 * k), replies.get(2 + 3 * k), replies.get(3 + 3 * k)) else { break };
        let Reply::Bytes { bytes: object, .. } = object else { truncated = false; break };
        let kind = object[0xa2];
        // Only kinds 1 and 5 are bonfires on the short component path the respawn uses.
        if kind != 1 && kind != 5 { continue; }
        let (Reply::Bytes { bytes: holder, .. }, Reply::Bytes { bytes: id, .. }) = (holder, id) else { continue };
        let axis = [f32_at(object, 0x60), f32_at(object, 0x64), f32_at(object, 0x68)];
        let translation = [f32_at(object, 0x70), f32_at(object, 0x74), f32_at(object, 0x78)];
        nodes.push(BonfireNode {
            id: format!("0x{:x}", u32::from_le_bytes(id[0..4].try_into().unwrap())),
            map: format!("{:08x}", u32::from_le_bytes(holder[8..12].try_into().unwrap())),
            kind,
            spawn: [0, 1, 2].map(|i| translation[i] - SPAWN_BEHIND * axis[i]),
        });
    }
    Ok(Bonfires { record, nodes, truncated })
}

pub fn bonfires(env: &Environment, instance: u8) -> Result<Bonfires, String> {
    let install = install(env, instance)?;
    let exchange = probe::request(install, &bonfire_commands(), Duration::from_secs(8));
    let answers = exchange.outcome.map_err(|reason| format!("{reason}: fogueiras"))?;
    decode_bonfires(&answers.iter().map(|a| &a.reply).collect::<Vec<_>>())
}

/// `0x7ba7`, `7ba7` or decimal digits read as hex, the way the channel prints ids.
pub fn parse_id(text: &str) -> Result<u32, String> {
    u32::from_str_radix(text.trim().trim_start_matches("0x"), 16).map_err(|_| format!("invalid_bonfire: {text:?}; use o id em hex, como 0x7ba7"))
}

pub fn parse_point(text: &str) -> Result<[f32; 3], String> {
    let values: Vec<f32> = text.split(',').map(|v| v.trim().parse::<f32>()).collect::<Result<_, _>>()
        .map_err(|_| format!("invalid_target: {text:?}; use x,y,z"))?;
    match values.as_slice() {
        [x, y, z] if values.iter().all(|v| v.is_finite()) => Ok([*x, *y, *z]),
        _ => Err(format!("invalid_target: {text:?}; use x,y,z")),
    }
}

pub fn command(env: &Environment, instance: u8, to: Option<String>, to_bonfire: Option<String>) -> Result<(), String> {
    let target = match (to, to_bonfire) {
        (Some(point), None) => parse_point(&point)?,
        (None, Some(id)) => {
            let id = parse_id(&id)?;
            let list = bonfires(env, instance)?;
            let wanted = format!("0x{id:x}");
            list.nodes.iter().find(|n| n.id == wanted).map(|n| n.spawn)
                .ok_or_else(|| format!("bonfire_not_loaded: a fogueira {wanted} não está na lista do mapa carregado; use goto-map"))?
        }
        _ => return Err("invalid_target: use --to x,y,z ou --to-bonfire <id>".into()),
    };
    let data = teleport(env, instance, target)?;
    crate::output::line(format_args!("conta {instance}: {} escritas, assentou a {:.2} m na horizontal e {:.2} m na vertical de ({:.3}, {:.3}, {:.3}), sem morte em 3 s",
        data["written"], data["across"].as_f64().unwrap_or(f64::NAN), data["upDown"].as_f64().unwrap_or(f64::NAN), target[0], target[1], target[2]));
    crate::output::data(data);
    Ok(())
}

pub fn bonfires_command(env: &Environment, instance: u8) -> Result<(), String> {
    let list = bonfires(env, instance)?;
    if let Some(r) = &list.record { crate::output::line(format_args!("registro: mapa {:08x} id 0x{:x}", r.map, r.id)); }
    for n in &list.nodes {
        crate::output::line(format_args!("  {} mapa {} tipo {} nascimento ({:.3}, {:.3}, {:.3})", n.id, n.map, n.kind, n.spawn[0], n.spawn[1], n.spawn[2]));
    }
    crate::output::data(json!({"instance": instance, "bonfires": list}));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn block(len: usize, fields: &[(usize, Vec<u8>)]) -> Vec<u8> {
        let mut bytes = vec![0u8; len];
        for (at, value) in fields { bytes[*at..*at + value.len()].copy_from_slice(value); }
        bytes
    }

    fn objects(chr_vtable: u64) -> Vec<Reply> {
        vec![
            Reply::Bytes { address: 0x1000, bytes: block(376, &[(0, chr_vtable.to_le_bytes().to_vec()), (0x90, floats(&[6.0, -18.5, 209.0]))]) },
            Reply::Bytes { address: 0x2000, bytes: block(96, &[]) },
            Reply::Bytes { address: 0x3000, bytes: block(464, &[]) },
            Reply::Bytes { address: 0x4000, bytes: block(624, &[(0, BODY_VFTABLE.to_le_bytes().to_vec())]) },
            Reply::Bytes { address: 0x5000, bytes: block(48, &[]) },
        ]
    }

    #[test]
    fn the_lengths_reach_every_field_written() {
        let lengths: Vec<usize> = read_commands().iter().map(|c| c.args.last().unwrap().parse().unwrap()).collect();
        for (_, object, offset, value) in WRITES {
            let size = if matches!(value, Value3::Still) { 16 } else { 12 };
            assert!(offset + size <= lengths[object], "+{offset:#x} em {object}");
        }
    }

    #[test]
    fn every_write_carries_the_bytes_that_were_read() {
        let replies = objects(CHR_VFTABLE);
        let writes = plan(&replies.iter().collect::<Vec<_>>(), [10.5, 5.92, -16.25]).unwrap();
        assert_eq!(writes.len(), 13);
        // Feet: where the character stood is what the injector must find.
        assert_eq!(writes[0].args, vec!["1090".to_owned(), hex(&floats(&[10.5, 5.92, -16.25])), hex(&floats(&[6.0, -18.5, 209.0]))]);
        // The body goes 5 cm above the feet; the fall controller's ground goes last.
        assert_eq!(writes[6].args[1], hex(&floats(&[10.5, 5.92 + BODY_ABOVE_FEET, -16.25])));
        assert_eq!(writes[12].args[0], "5020");
        assert!(writes.iter().all(|w| w.args.len() == 3 && w.args[1].len() == w.args[2].len()));
    }

    #[test]
    fn a_wrong_vtable_or_a_missing_object_writes_nothing() {
        let replies = objects(0x1410e0000);
        assert!(plan(&replies.iter().collect::<Vec<_>>(), [0.0; 3]).unwrap_err().starts_with("vtable_mismatch:"));
        let mut replies = objects(CHR_VFTABLE);
        replies[4] = Reply::ChainUnresolved;
        assert!(plan(&replies.iter().collect::<Vec<_>>(), [0.0; 3]).unwrap_err().starts_with("no_character:"));
    }

    #[test]
    fn one_refused_write_fails_the_teleport() {
        let ok = Reply::Poked { address: 1, before: None, wrote: true };
        let refused = Reply::PokeRefused { address: 2, expected: "00".into(), found: Some("01".into()) };
        assert_eq!(judge(&[("a", &ok), ("b", &ok)]), Ok(2));
        let error = judge(&[("a", &ok), ("g", &refused)]).unwrap_err();
        assert!(error.starts_with("teleport_refused: 1 de 2"), "{error}");
        assert!(judge(&[("a", &Reply::Poked { address: 1, before: None, wrote: false })]).is_err());
    }

    #[test]
    fn arrival_allows_the_push_out_of_a_bonfire_but_not_a_fall() {
        let target = [13.056, -6.167, 276.660];
        // Measured: the cathedral bonfire's spawn settled 0.84 m away.
        assert!(arrived_at([12.9715, -6.2151, 275.8268], target));
        assert!(!arrived_at([13.056, -12.0, 276.660], target));
        assert!(!arrived_at([6.1855, -18.5166, 209.0531], target));
    }

    #[test]
    fn bonfires_are_read_from_the_nodes_that_resolved() {
        let commands = bonfire_commands();
        assert_eq!(commands.len(), 1 + 3 * MAX_NODES);
        assert_eq!(commands[4].args[1], "70,58,8,60,8");
        let record = Reply::Bytes { address: 1, bytes: block(368, &[(0x164, 0x0a1f0000u32.to_le_bytes().to_vec()), (0x16c, 0x7ba7u32.to_le_bytes().to_vec())]) };
        let object = Reply::Bytes { address: 2, bytes: block(192, &[(0xa2, vec![1]), (0x60, floats(&[0.0, 0.0, 1.0])), (0x70, floats(&[6.0, -18.5, 210.0]))]) };
        let holder = Reply::Bytes { address: 3, bytes: block(12, &[(8, 0x0a1f0000u32.to_le_bytes().to_vec())]) };
        let id = Reply::Bytes { address: 4, bytes: 0x7ba7u32.to_le_bytes().to_vec() };
        let replies = vec![&record, &object, &holder, &id, &Reply::ChainUnresolved, &Reply::ChainUnresolved, &Reply::ChainUnresolved];
        let list = decode_bonfires(&replies).unwrap();
        assert_eq!(list.record, Some(crate::memory::Bonfire { map: 0x0a1f0000, id: 0x7ba7 }));
        assert_eq!(list.nodes.len(), 1);
        assert_eq!(list.nodes[0].id, "0x7ba7");
        assert_eq!(list.nodes[0].map, "0a1f0000");
        assert!((list.nodes[0].spawn[2] - 208.9).abs() < 1e-3);
        assert!(!list.truncated);
        assert_eq!(parse_id("0x7BA7"), Ok(0x7ba7));
        assert_eq!(parse_point("6.186,-18.517,209.053").map(|p| p[1]), Ok(-18.517));
        assert!(parse_point("1,2").is_err());
    }
}
