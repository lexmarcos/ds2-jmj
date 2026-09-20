//! The local character, read out of the game's memory in one request.
//!
//! Every offset is 1.03 / Calibrations 2.02 and `probe::request` refuses any
//! other executable. They are the ones `DS2_DeathInterceptHook` reads and
//! writes (HP, maximum, hollowing, deaths, the bonfire record) and the ones
//! `docs/DS2_SEAMLESS_COOP.md` measured for souls, role and hollow state.
//!
//! ```text
//! ctx    = *(base+0x16148f0)
//! chr    = *(ctx+0xd0)        +0x90 position (3 × f32), +0x168 HP, +0x174 maximum HP (after hollowing)
//! param  = *(chr+0x490)       +0xec souls, +0x1a4 deaths, +0x1ac hollowing (0..32)
//! roles  = *(chr+0xb0)        +0x3c role (0 world owner, 1 white phantom), +0x3e hollow state (0 human, 1 hollow)
//! record = *(ctx+0x70)        +0x164 bonfire map, +0x16c bonfire id
//! ```
//!
//! `chr` is rebuilt on every load, so an address read here is only good until
//! the next loading screen; anything that writes through it passes the bytes
//! it expects.

use std::time::Duration;

use serde::Serialize;

use crate::env::Install;
use crate::probe::{self, Command, Reply};

const CHR: &str = "chain chr 16148f0 d0 376";
const PARAM: &str = "chain param 16148f0 d0,490 432";
const ROLES: &str = "chain roles 16148f0 d0,b0 64";
const RECORD: &str = "chain record 16148f0 70 368";

pub const HP: u64 = 0x168;
const HP_MAX: usize = 0x174;
const POSITION: usize = 0x90;
const SOULS: usize = 0xec;
const DEATHS: usize = 0x1a4;
const HOLLOW: usize = 0x1ac;
const ROLE: usize = 0x3c;
const HOLLOW_STATE: usize = 0x3e;
const BONFIRE_MAP: usize = 0x164;
const BONFIRE_ID: usize = 0x16c;

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Character {
    /// The character object, good until the next load.
    pub address: String,
    pub hp: i32,
    pub hp_max: i32,
    pub souls: u32,
    pub deaths: i32,
    pub hollow: u8,
    pub hollow_state: u8,
    pub role: u8,
    pub position: [f32; 3],
    /// The bonfire a death sends the character to; `null` when the record does not resolve.
    pub bonfire: Option<Bonfire>,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct Bonfire { pub map: u32, pub id: u32 }

pub fn commands() -> Vec<Command> {
    [CHR, PARAM, ROLES, RECORD].iter().map(|line| Command::parse(line).expect("valid line")).collect()
}

/// One round trip. At the title or during a load there is no local character,
/// and that is an error of its own rather than a character full of zeros.
pub fn read(install: &Install, timeout: Duration) -> Result<Character, String> {
    let exchange = probe::request(install, &commands(), timeout);
    let answers = exchange.outcome.map_err(|reason| format!("{reason}: {}",
        exchange.detail.unwrap_or_else(|| "sem resposta completa do MemProbe".into())))?;
    let replies: Vec<&Reply> = answers.iter().map(|a| &a.reply).collect();
    decode(replies[0], replies[1], replies[2], replies[3])
}

fn field<const N: usize>(bytes: &[u8], at: usize) -> [u8; N] {
    bytes[at..at + N].try_into().expect("length checked by the request")
}

pub fn decode(chr: &Reply, param: &Reply, roles: &Reply, record: &Reply) -> Result<Character, String> {
    let bytes = |reply: &Reply, what: &str| match reply {
        Reply::Bytes { address, bytes } => Ok((*address, bytes.clone())),
        Reply::ChainUnresolved | Reply::Unreadable { .. } =>
            Err(format!("no_character: {what} não resolveu; sem personagem local (título ou carregamento)")),
        other => Err(format!("malformed_answer: {what}: {other:?}")),
    };
    let (address, chr) = bytes(chr, "personagem")?;
    let (_, param) = bytes(param, "PlayerParam")?;
    let (_, roles) = bytes(roles, "papel")?;
    let f32_at = |at: usize| f32::from_le_bytes(field(&chr, at));
    let character = Character {
        address: format!("0x{address:016x}"),
        hp: i32::from_le_bytes(field(&chr, HP as usize)),
        hp_max: i32::from_le_bytes(field(&chr, HP_MAX)),
        souls: u32::from_le_bytes(field(&param, SOULS)),
        deaths: i32::from_le_bytes(field(&param, DEATHS)),
        hollow: param[HOLLOW],
        hollow_state: roles[HOLLOW_STATE],
        role: roles[ROLE],
        position: [f32_at(POSITION), f32_at(POSITION + 4), f32_at(POSITION + 8)],
        bonfire: match record {
            Reply::Bytes { bytes, .. } => Some(Bonfire {
                map: u32::from_le_bytes(field(bytes, BONFIRE_MAP)),
                id: u32::from_le_bytes(field(bytes, BONFIRE_ID)),
            }),
            _ => None,
        },
    };
    // A freed or half-built object reads as garbage rather than failing.
    if character.hp_max <= 0 || character.hp < 0 || character.hp > character.hp_max || !character.position.iter().all(|v| v.is_finite()) {
        return Err(format!("implausible_character: hp {}/{} em {}", character.hp, character.hp_max, character.address));
    }
    Ok(character)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn block(len: usize, fields: &[(usize, &[u8])]) -> Vec<u8> {
        let mut bytes = vec![0u8; len];
        for (at, value) in fields { bytes[*at..*at + value.len()].copy_from_slice(value); }
        bytes
    }

    #[test]
    fn the_lengths_reach_every_field_read() {
        let lengths: Vec<usize> = commands().iter().map(|c| c.args.last().unwrap().parse().unwrap()).collect();
        assert!(lengths[0] >= HP_MAX + 4 && lengths[0] >= POSITION + 12);
        assert!(lengths[1] >= HOLLOW + 1 && lengths[1] >= DEATHS + 4);
        assert!(lengths[2] >= HOLLOW_STATE + 1);
        assert!(lengths[3] >= BONFIRE_ID + 4);
        // No leading 0 in any chain: the injector dereferences the module address first.
        assert!(commands().iter().all(|c| !c.args[1].starts_with('0')));
    }

    #[test]
    fn a_character_is_decoded_from_the_four_blocks() {
        let chr = Reply::Bytes { address: 0x7fffeb7b9e60, bytes: block(376, &[
            (POSITION, &[6.1855f32.to_le_bytes(), (-18.5166f32).to_le_bytes(), 209.0531f32.to_le_bytes()].concat()),
            (HP as usize, &869i32.to_le_bytes()), (HP_MAX, &869i32.to_le_bytes())]) };
        let param = Reply::Bytes { address: 1, bytes: block(432, &[(SOULS, &1234u32.to_le_bytes()), (DEATHS, &56i32.to_le_bytes()), (HOLLOW, &[1])]) };
        let roles = Reply::Bytes { address: 2, bytes: block(64, &[(ROLE, &[0]), (HOLLOW_STATE, &[1])]) };
        let record = Reply::Bytes { address: 3, bytes: block(368, &[(BONFIRE_MAP, &0x0a100000u32.to_le_bytes()), (BONFIRE_ID, &0x1234u32.to_le_bytes())]) };
        let character = decode(&chr, &param, &roles, &record).unwrap();
        assert_eq!((character.hp, character.hp_max, character.souls, character.deaths, character.hollow, character.hollow_state),
            (869, 869, 1234, 56, 1, 1));
        assert_eq!(character.address, "0x00007fffeb7b9e60");
        assert_eq!(character.bonfire, Some(Bonfire { map: 0x0a100000, id: 0x1234 }));
        assert!((character.position[1] + 18.5166).abs() < 1e-4);

        assert_eq!(decode(&chr, &param, &roles, &Reply::ChainUnresolved).unwrap().bonfire, None);
        assert!(decode(&Reply::ChainUnresolved, &param, &roles, &record).unwrap_err().starts_with("no_character:"));
        let garbage = Reply::Bytes { address: 9, bytes: block(376, &[(HP as usize, &900i32.to_le_bytes()), (HP_MAX, &10i32.to_le_bytes())]) };
        assert!(decode(&garbage, &param, &roles, &record).unwrap_err().starts_with("implausible_character:"));
    }
}
