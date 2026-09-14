//! What piles up across a day of tests, cleared where it is safe to clear.
//!
//! - **Hook logs.** The injector never rotates its own logs; the timer patch
//!   alone reached 412 MB, and a log that size is slow to excerpt and to read.
//!   `game prepare` rotates any `DS2*.log` above 8 MB to `.1` (one generation,
//!   the previous `.1` is replaced) for installations whose game is closed.
//! - **Rescue saves.** Every `save restore` keeps an `antes-de-*` copy of what it
//!   replaced; `save prune` keeps the newest few and never touches a label
//!   somebody chose.
//! - **Wine orphans.** Each stopped game can leave `xalia.exe` connected to X11
//!   until Xorg refuses new clients; `up` ends orphans when no game is open.
//!   `winedevice.exe` is **not** an orphan while its prefix's `services.exe`
//!   lives: it hosts the session's drivers — `winebus.sys`, the gamepad among
//!   them — and services does not bring it back. Killing the two of prefix 1
//!   on 14/09 left the next game there deaf to the pad, at the title screen.

use std::path::Path;
use std::time::{Duration, Instant, SystemTime};

use serde::Serialize;

pub const ROTATE_ABOVE: u64 = 8 << 20;

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Rotated {
    pub name: String,
    pub bytes: u64,
    /// `rotated`, `skipped_running` or `failed: <why>`.
    pub action: String,
}

fn is_hook_log(name: &str) -> bool { name.starts_with("DS2") && name.ends_with(".log") }

/// Renames every hook log above `threshold` to `<name>.1`. With the game open
/// nothing moves: the injector may hold the file, and a rename under it would
/// send its next lines into the `.1`.
pub fn rotate_logs(dir: &Path, threshold: u64, running: bool) -> Vec<Rotated> {
    let mut large: Vec<(String, u64)> = std::fs::read_dir(dir).into_iter().flatten().flatten().filter_map(|entry| {
        let name = entry.file_name().to_string_lossy().into_owned();
        let bytes = entry.metadata().ok().filter(|m| m.is_file())?.len();
        (is_hook_log(&name) && bytes > threshold).then_some((name, bytes))
    }).collect();
    large.sort();
    large.into_iter().map(|(name, bytes)| {
        let action = if running { "skipped_running".to_owned() } else {
            let from = dir.join(&name);
            let to = dir.join(format!("{name}.1"));
            match std::fs::rename(&from, &to) {
                Ok(()) => "rotated".to_owned(),
                Err(e) => format!("failed: {e}"),
            }
        };
        Rotated { name, bytes, action }
    }).collect()
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot { pub name: String, pub bytes: u64, #[serde(skip)] pub modified: SystemTime }

/// Whether a snapshot name is a copy `restore` made: `conta<N>-antes-de-<label>-`
/// followed by its stamp (`20260914-221512`) or its run id
/// (`1789424111010200483-554252`). A label somebody typed that happens to
/// start with `antes-de-` has neither, and is not a rescue.
pub fn is_rescue(name: &str) -> bool {
    let Some(stem) = name.strip_suffix(".ds3os") else { return false };
    let Some((account, rest)) = stem.split_once('-') else { return false };
    if !account.strip_prefix("conta").is_some_and(|n| !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit())) { return false; }
    let Some(label_and_suffix) = rest.strip_prefix("antes-de-") else { return false };
    let parts: Vec<&str> = label_and_suffix.rsplitn(3, '-').collect();
    let digits = |s: &str, len: Option<usize>| !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()) && len.is_none_or(|l| s.len() == l);
    matches!(parts.as_slice(), [last, first, label] if !label.is_empty()
        && ((digits(first, Some(8)) && digits(last, Some(6))) || (digits(first, Some(19)) && digits(last, None))))
}

/// Which of an account's snapshots `prune` removes: rescues named
/// `conta<N>-<pattern>…` beyond the `keep` newest by modification time. Names
/// alone do not order them — the rescue label changed format once already.
pub fn prune_plan(snapshots: &[Snapshot], account: u8, pattern: &str, keep: usize) -> (Vec<Snapshot>, Vec<Snapshot>) {
    let prefix = format!("conta{account}-{pattern}");
    let mut matching: Vec<Snapshot> = snapshots.iter().filter(|s| s.name.starts_with(&prefix) && is_rescue(&s.name)).cloned().collect();
    matching.sort_by(|a, b| b.modified.cmp(&a.modified).then_with(|| b.name.cmp(&a.name)));
    let delete = matching.split_off(keep.min(matching.len()));
    (delete, matching)
}

/// Rescue copies are the only thing prune may delete.
pub fn validate_pattern(pattern: &str) -> Result<(), String> {
    if !pattern.starts_with("antes-de-") || pattern.contains('/') {
        return Err(format!("pattern_not_rescue: {pattern:?} não começa com antes-de-; `save prune` só apaga as cópias de resgate de `restore`"));
    }
    Ok(())
}

pub fn snapshots(store: &Path) -> Vec<Snapshot> {
    std::fs::read_dir(store).into_iter().flatten().flatten().filter_map(|entry| {
        let meta = entry.metadata().ok().filter(|m| m.is_file())?;
        Some(Snapshot { name: entry.file_name().into_string().ok()?, bytes: meta.len(), modified: meta.modified().ok()? })
    }).collect()
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Orphan { pub pid: u32, pub name: String, pub wine_prefix: Option<String> }

/// Which Wine helpers are leftovers: `xalia.exe` in a prefix with no game
/// open, and `winedevice.exe` in a prefix whose `services.exe` is gone.
pub fn classify_orphans(processes: &[Orphan], game_prefixes: &[String]) -> Vec<Orphan> {
    let with_services: Vec<&str> = processes.iter().filter(|p| p.name == "services.exe")
        .filter_map(|p| p.wine_prefix.as_deref()).collect();
    let mut orphans: Vec<Orphan> = processes.iter().filter(|p| {
        let prefix = p.wine_prefix.as_deref();
        match p.name.as_str() {
            "xalia.exe" => !prefix.is_some_and(|x| game_prefixes.iter().any(|g| g == x)),
            "winedevice.exe" => !prefix.is_some_and(|x| with_services.contains(&x)),
            _ => false,
        }
    }).cloned().collect();
    orphans.sort_by_key(|o| o.pid);
    orphans
}

pub fn wine_orphans(every_game: &[u32]) -> Vec<Orphan> {
    let game_prefixes: Vec<String> = every_game.iter().filter_map(|pid| crate::proc::env_of(*pid, "WINEPREFIX"))
        .map(|p| p.trim_end_matches('/').to_owned()).collect();
    let processes: Vec<Orphan> = std::fs::read_dir("/proc").into_iter().flatten().flatten().filter_map(|entry| {
        let pid = entry.file_name().to_string_lossy().parse::<u32>().ok()?;
        let name = std::fs::read_to_string(format!("/proc/{pid}/comm")).ok()?.trim().to_owned();
        if !["xalia.exe", "winedevice.exe", "services.exe"].contains(&name.as_str()) { return None; }
        let wine_prefix = crate::proc::env_of(pid, "WINEPREFIX").map(|p| p.trim_end_matches('/').to_owned());
        Some(Orphan { pid, name, wine_prefix })
    }).collect();
    classify_orphans(&processes, &game_prefixes)
}

/// Connections to the X server, counted from `/proc/net/unix`.
pub fn x11_connections() -> Option<usize> {
    let sockets = std::fs::read_to_string("/proc/net/unix").ok()?;
    Some(sockets.lines().filter(|line| line.contains("/tmp/.X11-unix/X")).count())
}

fn alive(orphan: &Orphan) -> bool {
    std::fs::read_to_string(format!("/proc/{}/comm", orphan.pid)).is_ok_and(|comm| comm.trim() == orphan.name)
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Cleared { pub ended: Vec<Orphan>, pub survived: Vec<Orphan>, pub x11_before: Option<usize>, pub x11_after: Option<usize> }

/// Ends the orphans by pid — `pkill -f` matches the shell running it too —
/// and counts one as ended only once its pid no longer names that process.
pub fn clear_orphans(orphans: Vec<Orphan>) -> Cleared {
    let x11_before = x11_connections();
    for o in &orphans { unsafe { libc::kill(o.pid as i32, libc::SIGTERM); } }
    let wait = |orphans: &[Orphan]| {
        let until = Instant::now() + Duration::from_secs(3);
        while orphans.iter().any(alive) && Instant::now() < until { std::thread::sleep(Duration::from_millis(100)); }
    };
    wait(&orphans);
    for o in orphans.iter().filter(|o| alive(o)) { unsafe { libc::kill(o.pid as i32, libc::SIGKILL); } }
    wait(&orphans);
    let (survived, ended): (Vec<Orphan>, Vec<Orphan>) = orphans.into_iter().partition(alive);
    // Xorg drops a dead client's socket a moment later.
    std::thread::sleep(Duration::from_millis(300));
    Cleared { ended, survived, x11_before, x11_after: x11_connections() }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("ds2os-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn only_large_hook_logs_of_a_closed_game_rotate() {
        let dir = temp("rotate-logs");
        let big = vec![b'x'; 9 << 20];
        std::fs::write(dir.join("DS2_TimerParamPatch.log"), &big).unwrap();
        std::fs::write(dir.join("DS2_TimerParamPatch.log.1"), b"older").unwrap();
        std::fs::write(dir.join("DS2_MemProbe.log"), vec![b'x'; 7 << 20]).unwrap();
        std::fs::write(dir.join("DS2_MemProbe.req"), &big).unwrap();
        std::fs::write(dir.join("DS2_Nav.txt"), &big).unwrap();
        std::fs::write(dir.join("steam_api.log"), &big).unwrap();

        let open = rotate_logs(&dir, ROTATE_ABOVE, true);
        assert_eq!(open, vec![Rotated { name: "DS2_TimerParamPatch.log".into(), bytes: 9 << 20, action: "skipped_running".into() }]);
        assert!(dir.join("DS2_TimerParamPatch.log").exists());

        let closed = rotate_logs(&dir, ROTATE_ABOVE, false);
        assert_eq!(closed.len(), 1);
        assert_eq!(closed[0].action, "rotated");
        assert!(!dir.join("DS2_TimerParamPatch.log").exists());
        assert_eq!(std::fs::metadata(dir.join("DS2_TimerParamPatch.log.1")).unwrap().len(), 9 << 20);
        for untouched in ["DS2_MemProbe.log", "DS2_MemProbe.req", "DS2_Nav.txt", "steam_api.log"] {
            assert!(dir.join(untouched).exists(), "{untouched}");
        }
        // A `.1` is never rotated again.
        assert!(rotate_logs(&dir, ROTATE_ABOVE, false).is_empty());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn a_driver_host_of_a_live_wine_session_is_not_an_orphan() {
        let p = |pid, name: &str, prefix: &str| Orphan { pid, name: name.into(), wine_prefix: Some(prefix.into()) };
        let processes = vec![
            // Prefix 1 on 14/09: no game, but the session (and Steam under it) alive.
            p(36954, "services.exe", "/pfx1"), p(36957, "winedevice.exe", "/pfx1"), p(36982, "winedevice.exe", "/pfx1"),
            p(37001, "xalia.exe", "/pfx1"),
            // Prefix 2 with its game open.
            p(562240, "services.exe", "/pfx2"), p(562243, "winedevice.exe", "/pfx2"), p(562305, "xalia.exe", "/pfx2"),
            // A session that ended and left its driver host behind.
            p(900, "winedevice.exe", "/pfx3"),
        ];
        let orphans = classify_orphans(&processes, &["/pfx2".to_owned()]);
        assert_eq!(orphans.iter().map(|o| o.pid).collect::<Vec<_>>(), vec![900, 37001]);
    }

    fn snap(name: &str, age_s: u64) -> Snapshot {
        Snapshot { name: name.into(), bytes: 1, modified: SystemTime::UNIX_EPOCH + Duration::from_secs(1_000_000 - age_s) }
    }

    #[test]
    fn prune_keeps_the_newest_rescues_and_never_a_chosen_label() {
        let all = vec![
            snap("conta1-antes-de-pre-passo6-20260914-221512.ds3os", 10),
            snap("conta1-antes-de-pre-passo6-1789424111010200483-554252.ds3os", 5),
            snap("conta1-antes-de-x-20260913-101010.ds3os", 1000),
            snap("conta1-antes-de-y-20260912-101010.ds3os", 2000),
            snap("conta1-pre-passo6.ds3os", 99999),
            snap("conta1-teste-antes-de-algo.ds3os", 99999),
            snap("conta2-antes-de-pre-passo6-20260914-221512.ds3os", 99999),
            snap("conta1-antes-de-z.ds3os.tmp", 99999),
            snap("conta1-antes-de-nivel1.ds3os", 99999),
            snap("conta1-antes-de-20260913-101010.ds3os", 99999),
        ];
        let (delete, kept) = prune_plan(&all, 1, "antes-de-", 2);
        assert_eq!(kept.iter().map(|s| s.name.as_str()).collect::<Vec<_>>(),
            vec!["conta1-antes-de-pre-passo6-1789424111010200483-554252.ds3os", "conta1-antes-de-pre-passo6-20260914-221512.ds3os"]);
        assert_eq!(delete.iter().map(|s| s.name.as_str()).collect::<Vec<_>>(),
            vec!["conta1-antes-de-x-20260913-101010.ds3os", "conta1-antes-de-y-20260912-101010.ds3os"]);
        let (delete, _) = prune_plan(&all, 1, "antes-de-", 10);
        assert!(delete.is_empty());
        let (delete, _) = prune_plan(&all, 2, "antes-de-", 0);
        assert_eq!(delete.len(), 1);
        assert!(is_rescue("conta2-antes-de-hoje-cortado-20260912-225931.ds3os"));
        assert!(!is_rescue("conta2-antes-de-nivel1.ds3os"));
        assert!(!is_rescue("conta2-antes-de-pre-passo6-2026091-221512.ds3os"));
        assert!(validate_pattern("pre-").is_err());
        assert!(validate_pattern("").is_err());
        assert!(validate_pattern("antes-de-pre-passo6").is_ok());
    }
}
