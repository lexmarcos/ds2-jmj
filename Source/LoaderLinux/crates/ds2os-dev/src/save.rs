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

    // An explicit Steam ID binds the hexadecimal save directory as well.
    if let Some(id) = crate::settings::HarnessConfig::load().steam_ids.get(&install.account) {
        let expected = id.parse::<u64>().ok()?;
        found.retain(|p| p.parent().and_then(|p| p.file_name()).and_then(|s| s.to_str())
            .and_then(|s| u64::from_str_radix(s, 16).ok()) == Some(expected));
    }
    if found.len() != 1 { return None; }
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

    if chosen.len() != wanted.len() {
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

    #[test]
    fn failed_copy_preserves_the_live_save() {
        let dir = std::env::temp_dir().join(format!("ds2-save-{}", crate::output::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let live = dir.join("live");
        let source = dir.join("source");
        std::fs::write(&live, b"original").unwrap();
        std::fs::write(&source, b"").unwrap();
        assert!(super::copy(&source, &live).is_err());
        assert_eq!(std::fs::read(&live).unwrap(), b"original");
        std::fs::write(&source, b"baseline").unwrap();
        super::copy(&source, &live).unwrap();
        assert_eq!(std::fs::read(&live).unwrap(), b"baseline");
        std::fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn labels_cannot_escape_the_snapshot_store() {
        for label in ["", "../outside", "x/../../outside", "a.ds3os"] { assert!(super::validate_label(label).is_err()); }
        assert!(super::validate_label("majula-ready-01").is_ok());
    }
}

pub fn snapshot_path(account: u8, label: &str) -> PathBuf {
    store().join(format!("conta{account}-{label}.ds3os"))
}

pub fn validate_label(label: &str) -> Result<(), String> {
    if label.is_empty() || label.len() > 180 || !label.bytes().all(|b| b.is_ascii_alphanumeric() || b"_-".contains(&b)) {
        Err("invalid_label: use até 180 letras ASCII, números, _ ou - (sem caminho/extensão)".into())
    } else { Ok(()) }
}

fn copy(from: &Path, to: &Path) -> Result<u64, String> {
    if let Some(parent) = to.parent() { std::fs::create_dir_all(parent).map_err(|e| e.to_string())?; }
    let temporary = to.with_extension(format!("{}.tmp", crate::output::id()));
    let result = (|| -> std::io::Result<u64> {
        let mut input = std::fs::File::open(from)?;
        let mut out = std::fs::File::options().write(true).create_new(true).open(&temporary)?;
        let n = std::io::copy(&mut input, &mut out)?;
        if n == 0 { return Err(std::io::Error::other("save vazio")); }
        out.sync_all()?;
        std::fs::rename(&temporary, to)?;
        Ok(n)
    })();
    if result.is_err() { let _ = std::fs::remove_file(&temporary); }
    result.map_err(|e| format!("{} -> {}: {e}", from.display(), to.display()))
}

/// Copies each account's live save into the store.
pub fn backup(environment: &Environment, instance: &str, label: Option<&str>) -> Result<(), String> {
    let label = label.map(str::to_owned).unwrap_or_else(stamp);
    validate_label(&label)?;
    let chosen = installs(environment, instance)?;
    // Validate the entire set before writing the first snapshot.
    let mut plan = Vec::new();
    for install in chosen {
        if !crate::observe::processes(environment, install.account).is_empty() {
            return Err(format!("save_busy: conta {}; feche o jogo para obter um snapshot consistente", install.account));
        }
        let live = live_save(&install).ok_or_else(|| format!("save_missing_or_ambiguous: conta {}", install.account))?;
        let target = snapshot_path(install.account, &label);
        if target.exists() { return Err(format!("snapshot_exists: {}", target.display())); }
        plan.push((install.account, live, target));
    }
    let mut saved = Vec::new();
    for (account, live, target) in plan {
        let bytes = copy(&live, &target)?;
        println!("conta {account}: {} ({bytes} bytes)", target.display());
        saved.push(serde_json::json!({"instance": account, "label": label, "file": crate::output::fingerprint(&target)}));
    }
    crate::output::data(serde_json::json!({"snapshots": saved}));
    Ok(())
}

/// Lists what is in the store, and where each account's live save is.
pub fn list(environment: &Environment) -> Result<(), String> {
    let live: Vec<_> = environment.installs.iter().map(|i| serde_json::json!({"instance": i.account,
        "path": live_save(i)})).collect();
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
    let snapshots: Vec<_> = names.iter().filter_map(|name| {
        let stem = name.strip_suffix(".ds3os")?;
        let (account, label) = stem.strip_prefix("conta")?.split_once('-')?;
        Some(serde_json::json!({"instance": account.parse::<u8>().ok()?, "label": label, "rescue": crate::hygiene::is_rescue(name), "path": store().join(name)}))
    }).collect();
    crate::output::data(serde_json::json!({"live": live, "snapshots": snapshots}));

    if names.is_empty() {
        println!("  nenhum ainda; `ds2os-dev save backup` faz o primeiro");
    }
    let (rescues, chosen): (Vec<&String>, Vec<&String>) = names.iter().partition(|name| crate::hygiene::is_rescue(name));
    for name in chosen {
        println!("  {name}");
    }
    for install in &environment.installs {
        let prefix = format!("conta{}-antes-de-", install.account);
        let mine: Vec<&&String> = rescues.iter().filter(|n| n.starts_with(&prefix)).collect();
        if let Some(newest) = mine.iter().max_by_key(|n| std::fs::metadata(store().join(n.as_str())).and_then(|m| m.modified()).ok()) {
            println!("  conta {}: {} cópia(s) de resgate antes-de-*, a mais nova {newest}; `save prune` apaga as antigas", install.account, mine.len());
        }
    }
    Ok(())
}

/// This invocation's evidence directory name, as printed after "evidências:".
fn run_id() -> String {
    crate::output::dir().file_name().map(|n| n.to_string_lossy().into_owned()).filter(|n| validate_label(n).is_ok())
        .unwrap_or_else(stamp)
}

/// Removes the rescue copies `restore` leaves beyond the `keep` newest of each account.
pub fn prune(environment: &Environment, instance: &str, pattern: &str, keep: usize, dry_run: bool) -> Result<(), String> {
    crate::hygiene::validate_pattern(pattern)?;
    let chosen = installs(environment, instance)?;
    let all = crate::hygiene::snapshots(&store());
    let mut report = Vec::new();
    let mut failures = Vec::new();
    for install in &chosen {
        let (delete, kept) = crate::hygiene::prune_plan(&all, install.account, pattern, keep);
        let mut removed = Vec::new();
        for snapshot in &delete {
            if dry_run { continue; }
            match std::fs::remove_file(store().join(&snapshot.name)) {
                Ok(()) => removed.push(snapshot.name.clone()),
                Err(e) => failures.push(format!("{}: {e}", snapshot.name)),
            }
        }
        let bytes: u64 = delete.iter().map(|s| s.bytes).sum();
        println!("  conta {}: {} {} ({} MB), {} mantida(s)", install.account,
            if dry_run { "apagaria" } else { "apagou" }, if dry_run { delete.len() } else { removed.len() }, bytes >> 20, kept.len());
        for s in &delete { println!("    {} {}", if dry_run { "-" } else { "x" }, s.name); }
        report.push(serde_json::json!({"instance": install.account, "delete": delete, "removed": removed, "kept": kept, "bytes": bytes}));
    }
    crate::output::data(serde_json::json!({"pattern": pattern, "keep": keep, "dryRun": dry_run, "accounts": report, "errors": failures}));
    if failures.is_empty() { Ok(()) } else { Err(format!("prune_failed: {}", failures.join("; "))) }
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
    stop_first: Option<crate::game::StopGuard>,
) -> Result<(), String> {
    validate_label(label)?;
    let chosen = installs(environment, instance)?;

    let mut restored = Vec::new();
    for install in &chosen {
        let source = snapshot_path(install.account, label);
        if !source.is_file() || std::fs::metadata(&source).map(|m| m.len() == 0).unwrap_or(true) {
            return Err(format!(
                "conta {}: não achei {} — `ds2os-dev save list` mostra o que existe",
                install.account,
                source.display()
            ));
        }
    }

    let running: Vec<u8> = chosen.iter().map(|i| i.account)
        .filter(|account| !crate::observe::processes(environment, *account).is_empty()).collect();
    if let Some(&account) = running.first() {
        let Some(guard) = stop_first else {
            return Err(format!(
                "conta {account}: o jogo está aberto, e ele reescreve o save ao sair. \
Feche com `ds2os-dev game stop --instance {account}` ou repita com --stop"
            ));
        };
        println!("  fechando o jogo antes de restaurar: conta(s) {running:?}");
        crate::game::stop_instances(environment, &running, guard)?;
    }

    // Confirm every destination before changing any live save.
    for install in &chosen {
        live_save(install).ok_or_else(|| format!("save_missing_or_ambiguous: conta {}", install.account))?;
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
        // standing between a wrong label and a lost character. Named after the
        // run that made it, so its evidence directory says why.
        let rescue = snapshot_path(install.account, &format!("antes-de-{label}-{}", run_id()));
        copy(&live, &rescue)?;

        let bytes = copy(&source, &live)?;
        restored.push(serde_json::json!({"instance": install.account, "label": label, "source": source,
            "live": live, "rescue": rescue, "bytes": bytes}));
        println!(
            "  conta {}: {} restaurado ({} bytes); o anterior ficou em {}",
            install.account,
            source.display(),
            bytes,
            rescue.display()
        );
    }

    crate::output::data(serde_json::json!({"restored": restored}));
    Ok(())
}
