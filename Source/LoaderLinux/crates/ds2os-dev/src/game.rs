//! Preparing the game directory and running the instances.
//!
//! Both instances are started here, by running Proton directly. Steam does not
//! have to be the one to launch instance 1: `proton run` in the prefix Steam
//! already built for the game does the same thing, and it does it without a
//! human clicking Play. The wrapper written into the launch options stays
//! there for playing by hand.

use std::path::{Path, PathBuf};

use ds2os_core::config::InjectorConfig;
use serde::Serialize;

use crate::env::Environment;
use crate::paths;
use crate::proc;
use crate::server;

pub const APP_ID: u32 = 335300;
const BINARIES: [&str; 2] = ["Injector.dll", "Injector.exe"];

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Prepared {
    pub account: u8,
    pub probe_area: bool,
    pub game_dir: PathBuf,
    pub launch_options: String,
    pub injector_config: PathBuf,
    pub copied: Vec<String>,
    /// Hook logs above 8 MB moved to `.1`, or left because the game is open.
    pub rotated: Vec<crate::hygiene::Rotated>,
    pub timer_seconds: f64,
    pub timer_patch: bool,
}

/// Writes the injector config and puts the injector binaries beside the game.
///
/// The key comes straight from the server's public.key, normalised, because the
/// server compares it byte for byte and a stray line ending is enough to have
/// every login dropped in silence.
pub fn prepare(
    environment: &Environment,
    install: &crate::env::Install,
    timer_seconds: f64,
    timer_patch: bool,
    probe_area: bool,
    watch_reads: bool,
    area_address: Option<String>,
    probe_zone: bool,
    force_zone: bool,
    remove_fog: bool,
    auto_rematch: bool,
    seamless: bool,
) -> Result<Prepared, String> {
    let game_dir = install.game_dir.clone();
    let server_paths = environment
        .server
        .as_ref()
        .ok_or("o servidor não está compilado; rode `ds2os-dev doctor`")?;
    let injector_source = environment
        .injector_source
        .clone()
        .ok_or("não achei Injector.dll; rode `ds2os-dev doctor`")?;

    let running = compat_data(environment, install.account).map(|p| !instance_pids(&p).is_empty()).unwrap_or(false);
    let rotated = crate::hygiene::rotate_logs(&game_dir, crate::hygiene::ROTATE_ABOVE, running);

    let mut copied = Vec::new();
    for name in BINARIES {
        let source = injector_source.join(name);
        let target = game_dir.join(name);
        if !source.is_file() {
            return Err(format!("{} não existe", source.display()));
        }
        if source != target {
            std::fs::copy(&source, &target)
                .map_err(|e| format!("não consegui copiar {name}: {e}"))?;
            copied.push(name.to_owned());
        }
    }

    let config = InjectorConfig {
        ServerName: "ds2os-dev".to_owned(),
        ServerHostname: "127.0.0.1".to_owned(),
        ServerPublicKey: server::public_key(server_paths)?,
        ServerGameType: "DarkSouls2".to_owned(),
        ServerPort: 50050,
        EnableSeperateSaveFiles: true,
        DS2PatchPhantomTimers: timer_patch,
        DS2PhantomTimerSeconds: timer_seconds,
        DS2ProbeArea: probe_area,
        DS2WatchAreaReads: watch_reads,
        DS2AreaAddress: area_address.unwrap_or_default(),
        DS2ProbeMultiPlayZone: probe_zone,
        DS2ForceMultiPlayZone: force_zone,
        DS2RemovePhantomFog: remove_fog,
        DS2AutoRematch: auto_rematch,
        DS2SeamlessCoop: seamless,
        DS2PartyGuest: false,
        DS2PartyAccept: String::new(),
        DS2PartyPassword: String::new(),
        DS2ForcedZoneId: 103110,
    };
    let injector_config = config
        .write_to(&game_dir)
        .map_err(|e| format!("não consegui escrever o Injector.config: {e}"))?;

    // Proton launched outside Steam does not provide this, and the game needs
    // it to reach the running Steam client.
    std::fs::write(game_dir.join("steam_appid.txt"), APP_ID.to_string())
        .map_err(|e| format!("não consegui escrever steam_appid.txt: {e}"))?;

    let script = write_wrapper(install)?;

    Ok(Prepared {
        account: install.account,
        probe_area,
        launch_options: format!("{} %command%", shell_quote(&script)),
        game_dir,
        injector_config,
        copied,
        rotated,
        timer_seconds,
        timer_patch,
    })
}

/// Quotes a path for a Steam launch options line, which a shell parses.
fn shell_quote(path: &Path) -> String {
    let text = path.to_string_lossy();
    if text.chars().all(|c| c.is_ascii_alphanumeric() || "._-/".contains(c)) {
        text.into_owned()
    } else {
        format!("'{}'", text.replace('\'', r"'\''"))
    }
}

/// Writes the wrapper Steam runs in place of the game.
fn write_wrapper(install: &crate::env::Install) -> Result<PathBuf, String> {
    let game_dir = install.game_dir.clone();
    let exe_name = install
        .game_exe
        .as_ref()
        .and_then(|p| p.file_name())
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| "DarkSoulsII.exe".to_owned());

    let script = game_dir.join("ds2os-launch.sh");
    let body = format!(
        r#"#!/usr/bin/env bash
# Written by ds2os-dev. Steam calls this with Proton's full command line, whose
# last argument is the game executable. We swap that for Injector.exe and hand
# it the real executable, so the injector runs inside the same container and
# wineserver session as the game.
set -euo pipefail
here="$(cd -- "$(dirname -- "${{BASH_SOURCE[0]}}")" && pwd)"
injector="$here/Injector.exe"
args=("$@")
last=$((${{#args[@]}} - 1))
if [[ ! -f "$injector" || "${{args[$last]}}" != *"{exe_name}" ]]; then
  echo "ds2os: nada para injetar, iniciando o jogo sem modificar" >&2
  exec "${{args[@]}}"
fi
game="${{args[$last]}}"
args[$last]="$injector"
args+=("$game")
exec "${{args[@]}}"
"#
    );

    std::fs::write(&script, body).map_err(|e| format!("não consegui escrever o wrapper: {e}"))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755))
            .map_err(|e| format!("não consegui tornar o wrapper executável: {e}"))?;
    }
    Ok(script)
}

/// Starts one instance in a Proton prefix of its own, outside Steam.
///
/// Instance 1 runs in the prefix Steam built for the game, so its saves and
/// settings are the ones the player already has. Instance 2 gets a prefix of
/// the harness's own: the game refuses to run twice in one prefix, and a second
/// prefix is what lets two clients share a machine.
///
/// `second_steam` points at a home directory holding a second Steam client. The
/// game's session layer is peer to peer over Steam and keyed on the account's
/// steam id, so two instances sharing one account can never connect to each
/// other. Pointing this instance at a second logged-in client is what gives it
/// a distinct peer identity.
pub fn launch(
    environment: &Environment,
    account: u8,
    second_steam: Option<&std::path::Path>,
) -> Result<u32, String> {
    // Account 1 runs through Proton directly and account 2 through its own
    // Steam client. Not for elegance: account 2 has to come from that client or
    // it logs in as account 1, and account 1 cannot come from Steam at all
    // while Steam still believes the app is running, which it does for a while
    // after the other instance stops.
    if account == 1 {
        return launch_with_proton(environment, account, second_steam);
    }
    return launch_through_steam(environment, account, second_steam);
}

/// Proton directly, without Steam.
pub fn launch_with_proton(
    environment: &Environment,
    account: u8,
    second_steam: Option<&std::path::Path>,
) -> Result<u32, String> {
    if let Some(pid) = proc::running(&paths::instance_pid(account), "Injector.exe") {
        return Ok(pid);
    }

    let install = environment
        .installs
        .iter()
        .find(|install| install.account == account)
        .ok_or_else(|| format!("não achei a instalação da conta {account}"))?;

    let game_dir = install.game_dir.clone();
    let game_exe = install
        .game_exe
        .clone()
        .ok_or("não achei DarkSoulsII.exe")?;
    let proton = environment.proton.clone().ok_or("nenhum Proton instalado")?;
    let steam_root = environment.steam_root.clone().ok_or("Steam não encontrada")?;

    let prefix = compat_data(environment, account)?;
    std::fs::create_dir_all(&prefix).map_err(|e| e.to_string())?;
    paths::ensure_dirs().map_err(|e| e.to_string())?;

    // A second launch into a prefix something else still holds blocks on
    // pfx.lock, which has no timeout: it looks like a hang with no way out but
    // killing it. Refuse instead, and say what to do.
    let busy = instance_pids(&prefix);
    if !busy.is_empty() {
        return Err(format!(
            "o prefixo da instância {account} ainda tem {} processo(s) do jogo; \
             rode `ds2os-dev game stop --instance {account}` antes",
            busy.len()
        ));
    }

    let injector = game_dir.join("Injector.exe");
    if !injector.is_file() {
        return Err("Injector.exe não está na pasta do jogo; rode `ds2os-dev game prepare`".into());
    }

    // With a second Steam home, the client this instance talks to is the one
    // logged in as the other account, and HOME is what steamclient follows.
    let second_steam = if account == 1 { None } else { second_steam };
    let (client_root, home) = match second_steam {
        Some(home) => {
            let root = ds2os_core::steam::Steam::discover_in(home)
                .map_err(|e| format!("não achei uma Steam em {}: {e}", home.display()))?;
            (root.root().to_path_buf(), Some(home.to_path_buf()))
        }
        None => (steam_root, None),
    };

    let mut env = vec![
        ("STEAM_COMPAT_CLIENT_INSTALL_PATH", client_root.display().to_string()),
        ("STEAM_COMPAT_DATA_PATH", prefix.display().to_string()),
        ("SteamAppId", APP_ID.to_string()),
        ("SteamGameId", APP_ID.to_string()),
        // The injector relaxes a memory protection check when it sees this,
        // which it needs under Wine.
        ("WINEPREFIX", prefix.join("pfx").display().to_string()),
    ];
    if let Some(home) = &home {
        env.push(("HOME", home.display().to_string()));
    }

    let managed = proc::spawn(
        &proton.join("proton"),
        &["run", &injector.display().to_string(), &game_exe.display().to_string()],
        &game_dir,
        &env,
        &paths::instance_log(account),
        &paths::instance_pid(account),
        false,
    )
    .map_err(|e| format!("não consegui iniciar a instância {account}: {e}"))?;

    Ok(managed.pid)
}

/// Starts the second instance by asking its own Steam client to launch it.
///
/// **The second instance must come from the second Steam client.** The session
/// between two players is peer to peer over Steam and keyed on the account's
/// steam id, so two instances on one account can never reach each other: the
/// server even renames the second connection `<id>_1`, and every test between
/// them silently proves nothing. The two clients exist for exactly this.
///
/// Running Proton directly does not achieve it. Setting `HOME` points the Linux
/// side at the other client, but Proton overwrites
/// `STEAM_COMPAT_CLIENT_INSTALL_PATH` with the installation it was launched
/// from, and that is the path the Windows side of steamclient follows: the game
/// then logs in as the first account while everything else looks right.
/// Measured, both ways, on this machine.
fn launch_through_steam(
    environment: &Environment,
    account: u8,
    second_steam: Option<&std::path::Path>,
) -> Result<u32, String> {
    // Steam takes the gamepad for itself and hands it to the games it starts.
    // A game started outside Steam, in parallel with one started by it, simply
    // never sees the pad: the harness presses buttons into a window that
    // ignores them, and the only symptom is a game that sits at its title
    // screen while every command reports success. So both accounts come up the
    // same way, through their own client.
    let home = match account {
        1 => paths::home(),
        _ => second_steam
            .ok_or("nenhuma segunda Steam configurada; rode `ds2os-dev steam2 init`")?
            .to_path_buf(),
    };
    let steam = ds2os_core::steam::Steam::discover_in(&home)
        .map_err(|e| format!("não achei uma Steam em {}: {e}", home.display()))?;
    let root = steam.root().to_path_buf();

    if let Ok(prefix) = compat_data(environment, account) {
        let running = instance_pids(&prefix);
        if let Some(pid) = running.first() {
            return Ok(*pid);
        }
    }

    // Steam launches the game through whatever is in that account's launch
    // options. Without the wrapper there the game starts with no injector at
    // all, reaches FromSoftware's servers, and nothing in the harness would say
    // so until a test quietly measured nothing.
    if !wrapper_in_launch_options(&root) {
        return Err(format!(
            "a conta 2 não tem o wrapper nas opções de lançamento da Steam dela.\n               Abra a segunda Steam (`ds2os-dev steam2 run`), propriedades do Dark Souls II, \
             e cole:\n  {}",
            environment
                .installs
                .iter()
                .find(|install| install.account == 2)
                .map(|install| format!(
                    "'{}' %command%",
                    install.game_dir.join("ds2os-launch.sh").display()
                ))
                .unwrap_or_else(|| "<rode `ds2os-dev game prepare` primeiro>".to_owned())
        ));
    }

    let managed = proc::spawn(
        &root.join("steam.sh"),
        &["-applaunch", &APP_ID.to_string()],
        &root,
        &[("HOME", home.display().to_string())],
        &paths::instance_log(account),
        &paths::instance_pid(account),
        false,
    )
    .map_err(|e| format!("não consegui pedir à segunda Steam que abra o jogo: {e}"))?;

    Ok(managed.pid)
}

/// Whether any account in that Steam client launches the game through the
/// harness's wrapper.
fn wrapper_in_launch_options(root: &Path) -> bool {
    let Ok(users) = std::fs::read_dir(root.join("userdata")) else {
        return false;
    };
    users.flatten().any(|user| {
        std::fs::read(user.path().join("config/localconfig.vdf"))
            .map(|bytes| String::from_utf8_lossy(&bytes).contains("ds2os-launch.sh"))
            .unwrap_or(false)
    })
}

/// The Proton data directory one account plays in./// The Proton data directory one account plays in.
///
/// Account 1 plays in the one Steam built for the game, so its saves are the
/// player's own. Account 2 plays in one belonging to the harness, because the
/// game refuses to run twice in a single prefix.
pub fn compat_data(environment: &Environment, account: u8) -> Result<PathBuf, String> {
    environment
        .installs
        .iter()
        .find(|install| install.account == account)
        .and_then(|install| install.prefix.as_ref())
        .and_then(|pfx| pfx.parent())
        .map(Path::to_path_buf)
        .ok_or_else(|| {
            format!(
                "a conta {account} não tem prefixo Proton; abra o jogo uma vez pela Steam dela"
            )
        })
}

/// Every process of the game running in `compat_data`, whichever launched it.
///
/// Wine puts the prefix in the environment of everything it starts, which is
/// the only thing that tells one instance's processes from the other's: both
/// run the same executable, and only one of them was started by the harness.
pub fn instance_pids(compat_data: &Path) -> Vec<u32> {
    let prefix = compat_data.join("pfx");
    // Proton hands the prefix down with a trailing slash, and the harness
    // passes it without one. Comparing the two strings as they come back
    // matched nothing, every instance looked like it belonged to no prefix,
    // and "the window of account 2" quietly fell back to "the second window",
    // which was account 1 — so a command aimed at one instance drove the other.
    let prefix = trim_slash(&prefix.to_string_lossy());

    proc::pids_matching("DarkSoulsII.exe")
        .into_iter()
        .chain(proc::pids_matching("Injector.exe"))
        .filter(|pid| {
            proc::env_of(*pid, "WINEPREFIX")
                .map(|value| trim_slash(&value) == prefix)
                .unwrap_or(false)
        })
        .collect()
}

fn trim_slash(path: &str) -> String {
    path.trim_end_matches('/').to_owned()
}

/// What a stop does when the instance is in a live session.
///
/// Killing a client mid-session is an illegal disconnect, and the game counts
/// them in the save until the character can do nothing multiplayer at all.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StopGuard {
    /// Refuse with `session_live`; the default for every command.
    Refuse,
    /// `--force`: stop anyway, and say so in events.
    Force,
    /// A scenario's cleanup with a baseline: the save is restored afterwards,
    /// so the strike is thrown away with it. Recorded as `cleanup_kill_with_session`.
    Cleanup,
}

/// The decision, from what the channel said. A channel that does not answer
/// does not block: that is a game starting, hung, or on a DLL without the hook,
/// and a stop that refuses exactly when the game is unresponsive is useless.
/// Returns the event phase to record, if any.
pub fn stop_verdict(account: u8, live: &Result<bool, String>, guard: StopGuard) -> Result<Option<&'static str>, String> {
    match (live, guard) {
        (Ok(false), _) => Ok(None),
        (Ok(true), StopGuard::Refuse) => Err(format!(
            "session_live: a conta {account} está numa sessão, e fechar o jogo agora custa um strike no save; \
termine com `ds2os-dev session end` ou passe --force")),
        (Ok(true), StopGuard::Force) => Ok(Some("forced_with_session")),
        (Ok(true), StopGuard::Cleanup) => Ok(Some("cleanup_kill_with_session")),
        (Err(_), _) => Ok(Some("session_check_unknown")),
    }
}

fn running_pids(environment: &Environment, account: u8) -> Result<Vec<u32>, String> {
    let mut pids = instance_pids(&compat_data(environment, account)?);
    if let Some(pid) = proc::running(&paths::instance_pid(account), "Injector.exe") {
        pids.push(pid);
    }
    Ok(pids)
}

/// Asks every running instance in `accounts` about its session before any of
/// them is touched, so `--instance both` never closes one and then refuses the other.
fn guard_stops(environment: &Environment, accounts: &[u8], guard: StopGuard) -> Result<(), String> {
    for &account in accounts {
        if running_pids(environment, account)?.is_empty() { continue; }
        let Some(install) = environment.installs.iter().find(|i| i.account == account) else { continue };
        let live = crate::session::live(install, std::time::Duration::from_secs(3));
        let verdict = stop_verdict(account, &live, guard);
        let phase = match &verdict { Ok(phase) => *phase, Err(_) => Some("refused_session_live") };
        if let Some(phase) = phase {
            let kind = if phase == "cleanup_kill_with_session" { phase } else { "stop" };
            crate::output::event(kind, serde_json::json!({"instance": account, "phase": phase,
                "live": live.as_ref().ok(), "error": live.as_ref().err()}));
        }
        verdict?;
    }
    Ok(())
}

/// Stops each instance and does not return until its prefix is free. Refuses
/// all of them, before stopping any, when one is in a live session (see `StopGuard`).
pub fn stop_instances(environment: &Environment, accounts: &[u8], guard: StopGuard) -> Result<Vec<(u8, usize)>, String> {
    guard_stops(environment, accounts, guard)?;
    let mut stopped = Vec::new();
    for &account in accounts {
        let pids = running_pids(environment, account)?;
        if !pids.is_empty() {
            for pid in &pids {
                proc::stop(*pid);
            }
            if !proc::wait_gone(&pids, std::time::Duration::from_secs(30)) {
                return Err(format!(
                    "a instância {account} não morreu; um processo dela ainda segura o prefixo"
                ));
            }
        }
        stopped.push((account, pids.len()));
    }
    Ok(stopped)
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstancesStatus {
    /// Every DarkSoulsII.exe the machine is running, however it was started.
    pub game_processes: Vec<u32>,
    pub second_instance_pid: Option<u32>,
    pub wrapper_installed: bool,
    pub injector_ready: bool,
}

pub fn instances_status(environment: &Environment) -> InstancesStatus {
    let game_dir = environment.game_dir.clone();
    InstancesStatus {
        game_processes: proc::game_pids(),
        second_instance_pid: compat_data(environment, 2)
            .ok()
            .and_then(|prefix| instance_pids(&prefix).into_iter().next()),
        wrapper_installed: game_dir
            .as_ref()
            .map(|d| d.join("ds2os-launch.sh").is_file())
            .unwrap_or(false),
        injector_ready: game_dir
            .as_ref()
            .map(|d| BINARIES.iter().all(|n| d.join(n).is_file()) && d.join("Injector.config").is_file())
            .unwrap_or(false),
    }
}

/// Log the injector writes inside the game directory.
pub fn injector_log(environment: &Environment) -> Option<PathBuf> {
    Some(environment.game_dir.as_ref()?.join("DS2OS_Injector.log"))
}

/// Log the phantom timer patch writes inside the game directory. Both instances
/// append to it, which is fine: each line carries its own hit and patch counts.
pub fn timer_log(environment: &Environment) -> Option<PathBuf> {
    Some(environment.game_dir.as_ref()?.join("DS2_TimerParamPatch.log"))
}

pub fn exists(path: &Path) -> bool {
    path.is_file()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_live_session_refuses_unless_forced_or_cleaned_up() {
        let live = Ok(true);
        let error = stop_verdict(2, &live, StopGuard::Refuse).unwrap_err();
        assert!(error.starts_with("session_live:"), "{error}");
        assert_eq!(stop_verdict(2, &live, StopGuard::Force), Ok(Some("forced_with_session")));
        assert_eq!(stop_verdict(2, &live, StopGuard::Cleanup), Ok(Some("cleanup_kill_with_session")));
    }

    #[test]
    fn no_session_stops_quietly_and_an_unanswered_channel_does_not_block() {
        assert_eq!(stop_verdict(1, &Ok(false), StopGuard::Refuse), Ok(None));
        let silent = Err("request_not_consumed: nada leu DS2_Channel.req".to_owned());
        assert_eq!(stop_verdict(1, &silent, StopGuard::Refuse), Ok(Some("session_check_unknown")));
    }
}

/// `--party`: instance 2 keeps its white sign down, instance 1 summons the
/// signs of instance 2's configured Steam ID. Written over the config `prepare`
/// just wrote, so every other flag stays as given.
pub fn prepare_party(environment: &Environment, password: &str, host: u8) -> Result<Vec<serde_json::Value>, String> {
    let settings = crate::settings::HarnessConfig::load();
    let guest = if host == 1 { 2 } else { 1 };
    let guest_id = settings.steam_ids.get(&guest).cloned()
        .ok_or_else(|| format!("identity_missing: `ds2os-dev game identity --instance {guest} <SteamID64>` antes de --party"))?;
    let mut applied = Vec::new();
    for install in &environment.installs {
        let path = install.game_dir.join(ds2os_core::config::INJECTOR_CONFIG_FILE);
        let mut config: ds2os_core::config::InjectorConfig = serde_json::from_slice(
            &std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?).map_err(|e| format!("{}: {e}", path.display()))?;
        config.DS2PartyGuest = install.account == guest;
        config.DS2PartyAccept = if install.account == host { guest_id.clone() } else { String::new() };
        config.DS2PartyPassword = password.to_owned();
        config.write_to(&install.game_dir).map_err(|e| format!("{}: {e}", path.display()))?;
        applied.push(serde_json::json!({"instance": install.account, "guest": config.DS2PartyGuest, "accept": config.DS2PartyAccept,
            "password": !config.DS2PartyPassword.is_empty()}));
    }
    Ok(applied)
}
