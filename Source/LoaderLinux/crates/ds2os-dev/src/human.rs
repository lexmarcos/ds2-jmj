//! Burning a Human Effigy, the way the standing rule says: through Inventory,
//! and believed only when the character's own memory turns human.
//!
//! A hollow character cannot place a white sign or use the Cracked Red Eye Orb,
//! and the failure is a dead X with nothing in any log — so every staging burns
//! an effigy first. The belt's X has repeatedly done nothing where the menu
//! worked a minute later, and a belt count that does not move proves nothing.
//! What proves it is `param+0x1ac` (the hollowing level) and `roles+0x3e` (the
//! hollow state) both reading 0.
//!
//! The walk is blind, and it is only right from the menu's default place. The
//! start menu remembers its tab while the game runs (it wraps both ways, so no
//! number of presses reaches a known tab from an unknown one); a load resets it
//! to Equipment, and the Inventory grid always opens on its first item. So:
//!
//! start → right (Inventory) → A (categories, consumables first) → A (grid,
//! Estus) → right (Human Effigy) → A (Use highlighted) → A.
//!
//! Measured 14/09 on Samuel and Chico from a fresh boot. The effigy is the
//! second consumable in both saves; a different inventory order, or a menu
//! opened by hand since the last load, sends the same presses somewhere else.
//! That is why a screenshot is taken before the confirming A (evidence, not an
//! oracle), why a hollow state that does not change is a failure, and why the
//! walk ends by putting the tab back on Equipment — `game leave` walks from there
//! too.

use std::time::Duration;

use serde::Serialize;
use serde_json::{json, Value};

use crate::control::Deadline;
use crate::env::Environment;
use crate::memory::Character;
use crate::output;

/// Up to the confirming A, each press with the pause the menu needs after it.
pub const OPEN: [(&str, u64); 6] = [("press start", 1200), ("dpad right", 400), ("press a", 900), ("press a", 500),
    ("dpad right", 500), ("press a", 500)];
pub const CONFIRM: (&str, u64) = ("press a", 0);
/// Grid → categories → tab bar, tab back to Equipment, close.
pub const CLOSE: [(&str, u64); 4] = [("press b", 600), ("press b", 600), ("dpad left", 500), ("press b", 900)];
/// The item's use takes the menu a moment; a B sent right after the bytes flip
/// was swallowed once (14/09), the left then moved the category row instead of
/// the tab, and the menu stayed open on Inventory.
pub const SETTLE_MS: u64 = 1500;
/// After a failure the cursor's place is unknown; only back out.
pub const BACK_OUT: [(&str, u64); 3] = [("press b", 400), ("press b", 400), ("press b", 900)];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Verdict { AlreadyHuman, Human, StillHollow }

/// Human means both the level and the state read 0.
pub fn is_human(c: &Character) -> bool { c.hollow == 0 && c.hollow_state == 0 }

pub fn judge(before: &Character, after: Option<&Character>) -> Verdict {
    if is_human(before) { return Verdict::AlreadyHuman; }
    match after { Some(c) if is_human(c) => Verdict::Human, _ => Verdict::StillHollow }
}

fn walk(env: &Environment, instance: u8, steps: &[(&str, u64)], deadline: Deadline) -> Result<(), String> {
    for (command, pause) in steps {
        crate::drive::press(env, instance, command, deadline)?;
        if *pause > 0 { deadline.sleep(Duration::from_millis(*pause))?; }
    }
    Ok(())
}

fn summary(c: &Character) -> Value {
    json!({"hollow": c.hollow, "hollowState": c.hollow_state, "hp": c.hp, "hpMax": c.hp_max})
}

pub fn human(env: &Environment, instance: u8, timeout: Duration) -> Result<Value, String> {
    let deadline = Deadline::after(timeout);
    let install = crate::install_for(env, instance)?;
    let located = crate::probe::locate(install, Duration::from_secs(3));
    if located.state != crate::probe::Where::World {
        return Err(format!("not_in_world: conta {instance} está {} ({}); o menu só é conhecido no mundo", located.state, located.reason));
    }
    let before = crate::memory::read(install, Duration::from_secs(5))?;
    if judge(&before, None) == Verdict::AlreadyHuman {
        let data = json!({"instance": instance, "verdict": Verdict::AlreadyHuman, "before": summary(&before), "presses": 0});
        output::event("human", data.clone());
        return Ok(data);
    }

    walk(env, instance, &OPEN, deadline)?;
    let shot_dir = output::dir().join(format!("human-{instance}"));
    let evidence = crate::shot_into(env, Some(shot_dir.clone()), crate::screen::Scale::Half).err();
    walk(env, instance, &[CONFIRM], deadline)?;

    // The state flips within a second of the confirming A.
    let mut after = None;
    for _ in 0..10 {
        deadline.sleep(Duration::from_millis(500))?;
        if let Ok(c) = crate::memory::read(install, Duration::from_secs(3)) {
            let done = is_human(&c);
            after = Some(c);
            if done { break; }
        }
    }
    let verdict = judge(&before, after.as_ref());
    deadline.sleep(Duration::from_millis(SETTLE_MS))?;
    walk(env, instance, if verdict == Verdict::Human { &CLOSE } else { &BACK_OUT }, deadline)?;

    let data = json!({"instance": instance, "verdict": verdict, "before": summary(&before),
        "after": after.as_ref().map(summary), "screenshots": shot_dir, "screenshotError": evidence,
        "presses": OPEN.len() + 1 + if verdict == Verdict::Human { CLOSE.len() } else { BACK_OUT.len() }});
    output::event("human", data.clone());
    match verdict {
        Verdict::StillHollow => {
            output::data(data);
            Err(format!("still_hollow: conta {instance} continua hollow (nível {}, estado {}) depois do caminho do Inventário; \
veja a captura antes do A — um menu aberto à mão desde o último carregamento muda a aba",
                after.as_ref().map_or(before.hollow, |c| c.hollow), after.as_ref().map_or(before.hollow_state, |c| c.hollow_state)))
        }
        _ => Ok(data),
    }
}

pub fn command(env: &Environment, accounts: &[u8]) -> Result<(), String> {
    let mut results = Vec::new();
    for &account in accounts {
        let result = human(env, account, Duration::from_secs(40));
        match &result {
            Ok(data) => crate::output::line(format_args!("conta {account}: {}", match data["verdict"].as_str() {
                Some("already_human") => "já humano, nada apertado".to_owned(),
                _ => format!("humano (hollow {} -> 0, hp máximo {} -> {})", data["before"]["hollow"], data["before"]["hpMax"], data["after"]["hpMax"]),
            })),
            Err(e) => crate::output::line(format_args!("conta {account}: {e}")),
        }
        let failed = result.as_ref().err().cloned();
        results.push(result.unwrap_or_else(|e| json!({"instance": account, "error": e})));
        if let Some(e) = failed {
            output::data(json!({"instances": results}));
            return Err(e);
        }
    }
    output::data(json!({"instances": results}));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn character(hollow: u8, hollow_state: u8) -> Character {
        Character { address: "0x1".into(), hp: 777, hp_max: 777, souls: 0, deaths: 55, hollow, hollow_state, role: 0,
            position: [0.0; 3], bonfire: None }
    }

    #[test]
    fn only_both_bytes_at_zero_are_human() {
        assert_eq!(judge(&character(0, 0), None), Verdict::AlreadyHuman);
        assert_eq!(judge(&character(3, 1), Some(&character(0, 0))), Verdict::Human);
        assert_eq!(judge(&character(3, 1), Some(&character(0, 1))), Verdict::StillHollow);
        assert_eq!(judge(&character(3, 1), Some(&character(3, 1))), Verdict::StillHollow);
        assert_eq!(judge(&character(3, 1), None), Verdict::StillHollow);
    }

    #[test]
    fn the_walk_ends_with_the_tab_back_on_equipment() {
        let rights = OPEN.iter().filter(|(c, _)| *c == "dpad right").count();
        assert_eq!(rights, 2, "one to Inventory, one to the effigy");
        let lefts = CLOSE.iter().filter(|(c, _)| *c == "dpad left").count();
        // Two B's reach the tab bar, the left undoes the tab's right, the last B closes.
        assert_eq!((CLOSE[0].0, CLOSE[1].0, CLOSE[2].0, CLOSE[3].0), ("press b", "press b", "dpad left", "press b"));
        assert_eq!(lefts, 1);
        assert!(BACK_OUT.iter().all(|(c, _)| *c == "press b"));
        for (command, _) in OPEN.iter().chain(CLOSE.iter()).chain([CONFIRM].iter()) {
            crate::pad::validate(command).unwrap();
        }
    }
}
