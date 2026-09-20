//! The game's event flags, read from `EventFlagManager`.
//!
//! `mgr = *(*(*0x1416148f0 + 0x70) + 0x20)`; at `mgr + 0x20` a table of 31
//! buckets keyed by `((flag / 10000) * 0x89) % 31`, each node holding the
//! bytes at `+0x00`, their length at `+0x08`, the category (`flag / 10000`)
//! at `+0x0c` and the next node at `+0x10`. The bit is `flag % 10000`, most
//! significant first within a byte (`FUN_1404750b0`). See
//! docs/DS2_WORLD_STATE.md.

use std::collections::BTreeMap;
use std::time::Duration;

use crate::env::Install;
use crate::probe::{self, Command, Reply};

const MANAGER_VFTABLE: u64 = 0x1_410e_ff58;
const BUCKETS: usize = 31;
const MAX_NODES: usize = 64;
const MAX_GROUP_BYTES: u32 = 0x4000;

/// Every loaded category and its bytes.
pub type Groups = BTreeMap<u32, Vec<u8>>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Node { pub data: u64, pub length: u32, pub category: u32, pub next: u64 }

pub fn parse_manager(bytes: &[u8]) -> Result<Vec<u64>, String> {
    if bytes.len() < 0x20 + 8 * BUCKETS { return Err("flags_unreadable: gerenciador curto".into()); }
    let vftable = u64::from_le_bytes(bytes[0..8].try_into().unwrap());
    if vftable != MANAGER_VFTABLE {
        return Err(format!("flags_unexpected: vftable {vftable:#x}, esperado {MANAGER_VFTABLE:#x}"));
    }
    Ok((0..BUCKETS).map(|i| u64::from_le_bytes(bytes[0x20 + 8 * i..0x28 + 8 * i].try_into().unwrap())).filter(|&p| p != 0).collect())
}

pub fn parse_node(bytes: &[u8]) -> Option<Node> {
    (bytes.len() >= 24).then(|| Node {
        data: u64::from_le_bytes(bytes[0..8].try_into().unwrap()),
        length: u32::from_le_bytes(bytes[8..12].try_into().unwrap()),
        category: u32::from_le_bytes(bytes[12..16].try_into().unwrap()),
        next: u64::from_le_bytes(bytes[16..24].try_into().unwrap()),
    })
}

/// The flag ids whose bit is set in a category's bytes.
pub fn set_flags(category: u32, bytes: &[u8]) -> Vec<u64> {
    let mut out = Vec::new();
    for (i, byte) in bytes.iter().enumerate() {
        for bit in 0..8 {
            if byte & (0x80 >> bit) != 0 { out.push(category as u64 * 10000 + (i * 8 + bit) as u64); }
        }
    }
    out
}

/// One flag's value, or None when its category is not loaded or too short.
pub fn flag_value(groups: &Groups, flag: u64) -> Option<bool> {
    let bytes = groups.get(&u32::try_from(flag / 10000).ok()?)?;
    let bit = (flag % 10000) as usize;
    bytes.get(bit >> 3).map(|b| b & (0x80 >> (bit & 7)) != 0)
}

fn one(install: &Install, lines: &[String]) -> Result<Vec<Reply>, String> {
    let commands = lines.iter().map(|l| Command::parse(l)).collect::<Result<Vec<_>, _>>()?;
    let exchange = probe::request(install, &commands, Duration::from_secs(5));
    match exchange.outcome {
        Ok(answers) => Ok(answers.into_iter().map(|a| a.reply).collect()),
        Err(reason) => Err(format!("{reason}: {}", exchange.detail.unwrap_or_else(|| "sem resposta".into()))),
    }
}

fn bytes_of(reply: &Reply, what: &str) -> Result<Vec<u8>, String> {
    match reply {
        Reply::Bytes { bytes, .. } => Ok(bytes.clone()),
        other => Err(format!("flags_unreadable: {what}: {other:?}")),
    }
}

/// Reads every loaded category, one round trip per level of the lists.
pub fn read(install: &Install) -> Result<Groups, String> {
    let manager = one(install, &["chain m 16148f0 70,20 312".to_string()])?;
    let mut pending = parse_manager(&bytes_of(&manager[0], "gerenciador")?)?;
    let mut nodes = Vec::new();
    while !pending.is_empty() {
        if nodes.len() + pending.len() > MAX_NODES { return Err("flags_unexpected: nós demais".into()); }
        let lines: Vec<String> = pending.iter().enumerate().map(|(i, p)| format!("abs n{i} {p:x} 24")).collect();
        let replies = one(install, &lines)?;
        pending.clear();
        for reply in &replies {
            let node = parse_node(&bytes_of(reply, "nó")?).ok_or("flags_unreadable: nó curto")?;
            if node.next != 0 { pending.push(node.next); }
            nodes.push(node);
        }
    }
    let wanted: Vec<&Node> = nodes.iter().filter(|n| n.length > 0 && n.length <= MAX_GROUP_BYTES && n.data != 0).collect();
    let mut groups = Groups::new();
    if wanted.is_empty() { return Ok(groups); }
    let lines: Vec<String> = wanted.iter().enumerate().map(|(i, n)| format!("abs d{i} {:x} {}", n.data, n.length)).collect();
    let replies = one(install, &lines)?;
    for (node, reply) in wanted.iter().zip(&replies) {
        groups.insert(node.category, bytes_of(reply, "bytes da categoria")?);
    }
    Ok(groups)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_manager_with_another_vftable_is_refused() {
        let mut bytes = vec![0u8; 312];
        assert!(parse_manager(&bytes).is_err());
        bytes[0..8].copy_from_slice(&MANAGER_VFTABLE.to_le_bytes());
        bytes[0x20 + 8 * 6..0x28 + 8 * 6].copy_from_slice(&0x7fff_e808_bd00u64.to_le_bytes());
        assert_eq!(parse_manager(&bytes).unwrap(), vec![0x7fff_e808_bd00]);
    }

    #[test]
    fn bits_are_read_most_significant_first() {
        // 13100's bytes as both characters had them in Heide, 15/09.
        let bytes = [0x00, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0x02];
        assert_eq!(set_flags(13100, &bytes), vec![131000022, 131000086]);
        let groups = Groups::from([(13100, bytes.to_vec())]);
        assert_eq!(flag_value(&groups, 131000022), Some(true));
        assert_eq!(flag_value(&groups, 131000023), Some(false));
        assert_eq!(flag_value(&groups, 131009999), None);
        assert_eq!(flag_value(&groups, 100600), None);
    }

    #[test]
    fn a_node_is_pointer_length_category_next() {
        let mut b = Vec::new();
        b.extend_from_slice(&0x7fff_e80e_2af8u64.to_le_bytes());
        b.extend_from_slice(&25u32.to_le_bytes());
        b.extend_from_slice(&13100u32.to_le_bytes());
        b.extend_from_slice(&0u64.to_le_bytes());
        assert_eq!(parse_node(&b), Some(Node { data: 0x7fff_e80e_2af8, length: 25, category: 13100, next: 0 }));
    }
}
