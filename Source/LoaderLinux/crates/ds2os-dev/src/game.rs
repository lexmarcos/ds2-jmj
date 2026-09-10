//! Preparing the game directory and running the instances.
//!
//! Instance 1 is whatever Steam launches: the harness can configure it and
//! notice when it appears, but it cannot start it, because Steam owns that.
//! Instance 2 the harness starts itself, in its own Proton prefix.

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
    expand_slots: bool,
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
        DS2ForcedZoneId: 103110,
        DS2ExpandSessionSlots: expand_slots,
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

/// Starts the second instance in its own Proton prefix, outside Steam.
///
/// `second_steam` points at a home directory holding a second Steam client. The
/// game's session layer is peer to peer over Steam and keyed on the account's
/// steam id, so two instances sharing one account can never connect to each
/// other. Pointing this instance at a second logged-in client is what gives it
/// a distinct peer identity.
pub fn launch_second(
    environment: &Environment,
    second_steam: Option<&std::path::Path>,
) -> Result<u32, String> {
    if let Some(pid) = proc::running(&paths::instance_pid(2), "Injector.exe") {
        return Ok(pid);
    }

    let game_dir = environment.game_dir.clone().ok_or("Dark Souls II não está instalado")?;
    let game_exe = environment.game_exe.clone().ok_or("não achei DarkSoulsII.exe")?;
    let proton = environment.proton.clone().ok_or("nenhum Proton instalado")?;
    let steam_root = environment.steam_root.clone().ok_or("Steam não encontrada")?;

    let prefix = paths::second_prefix();
    std::fs::create_dir_all(&prefix).map_err(|e| e.to_string())?;
    paths::ensure_dirs().map_err(|e| e.to_string())?;

    let injector = game_dir.join("Injector.exe");
    if !injector.is_file() {
        return Err("Injector.exe não está na pasta do jogo; rode `ds2os-dev game prepare`".into());
    }

    // With a second Steam home, the client this instance talks to is the one
    // logged in as the other account, and HOME is what steamclient follows.
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
        &paths::instance_log(2),
        &paths::instance_pid(2),
        false,
    )
    .map_err(|e| format!("não consegui iniciar a segunda instância: {e}"))?;

    Ok(managed.pid)
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
        game_processes: proc::pids_matching("DarkSoulsII.exe"),
        second_instance_pid: proc::running(&paths::instance_pid(2), "Injector.exe"),
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

pub fn stop_second() -> bool {
    match proc::running(&paths::instance_pid(2), "Injector.exe") {
        Some(pid) => proc::stop(pid),
        None => true,
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
