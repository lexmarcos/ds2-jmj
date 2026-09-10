//! Starting the game with the injector attached.
//!
//! The three injector files have to share one directory, because the launcher
//! resolves the DLL beside itself and the DLL resolves its config beside
//! itself. That directory is the loader's, not the game's — see `paths`.
//!
//! On Windows the launcher is run directly. On Linux it has to run *inside*
//! the Wine prefix, because a native process cannot inject into a Wine one, so
//! Proton runs it. The command is the one the development harness has been
//! using all along, with the prefix changed from a throwaway to the game's
//! own so the player keeps their saves and settings.

use std::io;
use std::path::{Path, PathBuf};
use std::process::Child;

use crate::paths;
use crate::pinned;
use crate::preflight::Ready;

/// Files the injector writes beside its DLL, which have to be cleared before
/// each run so a tail cannot read the last session's lines as this session's.
/// One of these reached 119 MB during development.
const LEFTOVERS: [&str; 7] = [
    "DS2OS_Injector.log",
    "DS2_TimerParamPatch.log",
    "DS2_UnlockAreas.log",
    "DS2_MultiPlayZone.log",
    "DS2_AreaProbe.log",
    "DS2_MemProbe.req",
    "DS2_Trace.req",
];

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("não consegui preparar {path}: {source}")]
    Prepare { path: PathBuf, source: io::Error },
    #[error("não consegui iniciar o jogo: {0}")]
    Spawn(io::Error),
}

/// Puts everything in place, then starts the game.
pub fn start(ready: &Ready) -> Result<Child, Error> {
    prepare(ready)?;
    platform::spawn(ready).map_err(Error::Spawn)
}

/// Stages the injector, clears the last run's files, writes the config.
fn prepare(ready: &Ready) -> Result<(), Error> {
    let target = paths::injector_dir();
    let failed = |path: &Path| {
        let path = path.to_path_buf();
        move |source| Error::Prepare { path: path.clone(), source }
    };

    std::fs::create_dir_all(&target).map_err(failed(&target))?;
    std::fs::create_dir_all(paths::logs_dir()).map_err(failed(&paths::logs_dir()))?;

    for name in ["Injector.exe", "Injector.dll"] {
        let from = ready.injector_source.join(name);
        let to = target.join(name);
        if from == to {
            continue;
        }
        // Only when it would change anything: on Windows the file may still be
        // mapped by a game that has not finished exiting.
        let same = std::fs::metadata(&from)
            .and_then(|source| std::fs::metadata(&to).map(|dest| source.len() == dest.len()))
            .unwrap_or(false);
        if !same {
            std::fs::copy(&from, &to).map_err(failed(&to))?;
        }
    }

    for name in LEFTOVERS {
        let path = target.join(name);
        if path.exists() {
            std::fs::remove_file(&path).map_err(failed(&path))?;
        }
    }

    pinned::injector_config()
        .write_to(&target)
        .map_err(failed(&target))?;

    // The Steam API looks for this in the working directory, and the launcher
    // runs the game from the directory the executable is in. The install root
    // already has one, which is why launching through Steam works and a direct
    // launch would not.
    if let Some(game_dir) = ready.install.game_dir() {
        let stamp = game_dir.join("steam_appid.txt");
        let wanted = ready.install.app_id.to_string();
        let current = std::fs::read_to_string(&stamp).unwrap_or_default();
        if current.trim() != wanted {
            std::fs::write(&stamp, &wanted).map_err(failed(&stamp))?;
        }
    }

    Ok(())
}

/// A file the loader's own output goes to, truncated per run.
fn log_file(name: &str) -> io::Result<std::fs::File> {
    std::fs::File::create(paths::logs_dir().join(name))
}

#[cfg(unix)]
mod platform {
    use std::os::unix::process::CommandExt;
    use std::process::{Child, Command, Stdio};

    use super::*;

    pub fn spawn(ready: &Ready) -> io::Result<Child> {
        let proton = ready
            .proton
            .as_ref()
            .ok_or_else(|| io::Error::other("nenhum Proton resolvido"))?;

        let game_dir = ready
            .install
            .game_dir()
            .ok_or_else(|| io::Error::other("não achei a pasta do executável"))?;

        let compatdata = ready
            .install
            .compatdata_dir()
            .ok_or_else(|| io::Error::other("o jogo ainda não tem prefixo do Proton"))?;

        let mut command = Command::new(proton.launcher());
        command
            .arg("run")
            .arg(paths::injector_dir().join("Injector.exe"))
            .arg(&ready.game_exe)
            .current_dir(&game_dir)
            .env("STEAM_COMPAT_CLIENT_INSTALL_PATH", &ready.steam_root)
            .env("STEAM_COMPAT_DATA_PATH", &compatdata)
            .env("SteamAppId", ready.install.app_id.to_string())
            .env("SteamGameId", ready.install.app_id.to_string())
            // The injector relaxes a memory protection check when it sees
            // this, which it needs under Wine: VirtualQuery does not report
            // the protection Windows would.
            .env("WINEPREFIX", compatdata.join("pfx"))
            .stdin(Stdio::null())
            .stdout(log_file("proton.log")?)
            .stderr(log_file("proton.err")?)
            // Its own process group, so closing the loader — or a Ctrl-C in
            // the terminal it was started from — does not take the game down.
            .process_group(0);

        command.spawn()
    }
}

#[cfg(windows)]
mod platform {
    use std::os::windows::process::CommandExt;
    use std::process::{Child, Command, Stdio};

    use super::*;

    /// The launcher is a console program, and without this it flashes a
    /// console window over the game.
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;

    pub fn spawn(ready: &Ready) -> io::Result<Child> {
        let game_dir = ready
            .install
            .game_dir()
            .ok_or_else(|| io::Error::other("não achei a pasta do executável"))?;

        let mut command = Command::new(paths::injector_dir().join("Injector.exe"));
        command
            .arg(&ready.game_exe)
            .current_dir(&game_dir)
            .env("SteamAppId", ready.install.app_id.to_string())
            .env("SteamGameId", ready.install.app_id.to_string())
            // Only meaningful under Wine, and the injector changes behaviour
            // when it is set, so make sure a stray one cannot reach it.
            .env_remove("WINEPREFIX")
            .stdin(Stdio::null())
            .stdout(log_file("injector.log")?)
            .stderr(Stdio::null())
            .creation_flags(CREATE_NO_WINDOW);

        command.spawn()
    }
}
