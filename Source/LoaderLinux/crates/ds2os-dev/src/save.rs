//! The characters' save files, and putting one back.
//!
//! This exists because of a cost the co-op work only discovered by paying it
//! twice. Dark Souls II counts *illegal multiplayer disconnects*, and the
//! seamless-respawn experiment is one by the game's own reckoning: the guest
//! refuses the session teardown and then leaves. After a few, the client says
//!
//! > Due to repeated illegal multiplayer disconnects, your connection to other
//! > worlds was lost. Only a Bone of Order can restore your connection.
//!
//! and that character can no longer place a summon sign, use an orb, or even
//! ask the server for the sign list — so swapping who hosts does not get
//! around it. The in-game cure is an item there are only a few of, and both
//! characters spent theirs in one afternoon.
//!
//! The cure that scales is this file. The private server keeps its own save
//! (`EnableSeperateSaveFiles`), one per account, so a test can be undone by
//! copying a file back. Snapshot before, restore after, and the ban stops
//! being a budget.
//!
//! Two rules the code enforces rather than documents, because forgetting
//! either costs the save:
//!
//! - **the client must be stopped first.** A running game rewrites the file on
//!   its way out, so a restore under a live client is undone minutes later and
//!   looks like the restore silently failed.
//! - **a restore snapshots what it is about to overwrite.** Undo has to be
//!   available for the undo.

use std::path::{Path, PathBuf};

use crate::env::{Environment, Install};
use crate::paths;
use crate::proc;

/// Name the private server's save carries. The retail save sits beside it as
/// `DS2SOFS0000.sl2` and is never touched: keeping them apart is the whole
/// point of `EnableSeperateSaveFiles`.
const SAVE_NAME: &str = "DS2SOFS0000.ds3os";

/// Where snapshots live, beside the logs.
pub fn store() -> PathBuf {
    paths::state_dir().join("saves")
}

/// The live save for one account.
///
/// The Steam id is a directory in the prefix and the harness has no reason to
/// know it, so it is discovered rather than configured - which also means a
/// third account would work without a code change.
pub fn live_save(install: &Install) -> Option<PathBuf> {
    let roaming = install
        .prefix
        .as_ref()?
        .join("drive_c/users/steamuser/AppData/Roaming/DarkSoulsII");

    let mut found: Vec<PathBuf> = std::fs::read_dir(&roaming)
        .ok()?
        .flatten()
        .map(|entry| entry.path().join(SAVE_NAME))
        .filter(|candidate| candidate.is_file())
        .collect();

    // More than one would mean more than one Steam id has played in this
    // prefix. Newest wins, and `list` shows the path so it is visible.
    found.sort_by_key(|path| {
        std::fs::metadata(path)
            .and_then(|m| m.modified())
            .ok()
    });
    found.pop()
}

fn installs(environment: &Environment, instance: &str) -> Result<Vec<Install>, String> {
    let wanted: Vec<u8> = match instance {
        "both" => vec![1, 2],
        "1" => vec![1],
        "2" => vec![2],
        other => return Err(format!("instância '{other}' não existe; use 1, 2 ou both")),
    };

    let chosen: Vec<Install> = environment
        .installs
        .iter()
        .filter(|install| wanted.contains(&install.account))
        .cloned()
        .collect();

    if chosen.is_empty() {
        return Err("nenhuma instalação encontrada para essa instância".into());
    }
    Ok(chosen)
}

/// A default label, in UTC, that reads like a date and sorts like one.
///
/// These names are picked off a list by a person, so "20708-225124" will not
/// do; and pulling in a date crate for one line of arithmetic will not either.
/// The conversion is the standard civil-from-days one.
fn stamp() -> String {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0) as i64;

    let (y, m, d) = civil_from_days(now.div_euclid(86_400));
    let secs = now.rem_euclid(86_400);
    format!(
        "{y:04}{m:02}{d:02}-{:02}{:02}{:02}",
        secs / 3600,
        (secs % 3600) / 60,
        secs % 60
    )
}

fn civil_from_days(days: i64) -> (i64, i64, i64) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    (if month <= 2 { year + 1 } else { year }, month, day)
}

#[cfg(test)]
mod tests {
    use super::civil_from_days;

    #[test]
    fn known_days_convert() {
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(civil_from_days(19_723), (2024, 1, 1));
        // A leap day, because that is where this arithmetic goes wrong.
        assert_eq!(civil_from_days(19_782), (2024, 2, 29));
    }
}

fn snapshot_path(account: u8, label: &str) -> PathBuf {
    store().join(format!("conta{account}-{label}.ds3os"))
}

fn copy(from: &Path, to: &Path) -> Result<u64, String> {
    if let Some(parent) = to.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
    }
    std::fs::copy(from, to).map_err(|e| format!("{} -> {}: {e}", from.display(), to.display()))
}

/// Copies each account's live save into the store.
pub fn backup(environment: &Environment, instance: &str, label: Option<&str>) -> Result<(), String> {
    let label = label.map(str::to_owned).unwrap_or_else(stamp);

    for install in installs(environment, instance)? {
        match live_save(&install) {
            Some(live) => {
                let target = snapshot_path(install.account, &label);
                let bytes = copy(&live, &target)?;
                println!(
                    "  conta {}: {} guardado ({} bytes)",
                    install.account,
                    target.display(),
                    bytes
                );
            }
            None => println!(
                "  conta {}: nenhum save do servidor privado ainda; nada a guardar",
                install.account
            ),
        }
    }
    Ok(())
}

/// Lists what is in the store, and where each account's live save is.
pub fn list(environment: &Environment) -> Result<(), String> {
    println!("ao vivo");
    for install in &environment.installs {
        match live_save(install) {
            Some(path) => {
                let size = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
                println!("  conta {}: {} ({} bytes)", install.account, path.display(), size);
            }
            None => println!("  conta {}: ainda não existe", install.account),
        }
    }

    println!("\nguardados em {}", store().display());
    let mut names: Vec<String> = std::fs::read_dir(store())
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| entry.file_name().into_string().ok())
        .filter(|name| name.ends_with(".ds3os"))
        .collect();
    names.sort();

    if names.is_empty() {
        println!("  nenhum ainda; `ds2os-dev save backup` faz o primeiro");
    }
    for name in names {
        println!("  {name}");
    }
    Ok(())
}

/// Puts a snapshot back over the live save.
///
/// Refuses while the client is running unless asked to stop it, because a
/// running game rewrites the file when it exits and the restore would be
/// quietly undone.
pub fn restore(
    environment: &Environment,
    instance: &str,
    label: &str,
    stop_first: bool,
) -> Result<(), String> {
    let chosen = installs(environment, instance)?;

    for install in &chosen {
        let source = snapshot_path(install.account, label);
        if !source.is_file() {
            return Err(format!(
                "conta {}: não achei {} — `ds2os-dev save list` mostra o que existe",
                install.account,
                source.display()
            ));
        }
    }

    for install in &chosen {
        let running = proc::running(&paths::instance_pid(install.account), "Injector.exe").is_some()
            || !proc::game_pids().is_empty();

        if running {
            if !stop_first {
                return Err(format!(
                    "conta {}: o jogo está aberto, e ele reescreve o save ao sair. \
Feche com `ds2os-dev game stop --instance {}` ou repita com --stop",
                    install.account, install.account
                ));
            }
            println!("  conta {}: fechando o jogo antes de restaurar", install.account);
            crate::game::stop_instance(environment, install.account)?;
        }
    }

    for install in &chosen {
        let source = snapshot_path(install.account, label);
        let live = live_save(install).ok_or_else(|| {
            format!(
                "conta {}: não achei o save ao vivo para substituir",
                install.account
            )
        })?;

        // The undo needs an undo. This is cheap and it is the only thing
        // standing between a wrong label and a lost character.
        let rescue = snapshot_path(install.account, &format!("antes-de-{label}-{}", stamp()));
        copy(&live, &rescue)?;

        let bytes = copy(&source, &live)?;
        println!(
            "  conta {}: {} restaurado ({} bytes); o anterior ficou em {}",
            install.account,
            source.display(),
            bytes,
            rescue.display()
        );
    }

    Ok(())
}
