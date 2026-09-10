//! Everything that has to be true before the game can start.
//!
//! The checks run in order and the first failure is what the player is told,
//! because a list of six problems is not more helpful than the one they have
//! to fix first.
//!
//! Two of them are worth explaining, because they look like fussiness and are
//! not:
//!
//! The build check is a hard block. `DS2_ReplaceServerAddressHook` searches
//! for the retail hostname inside a `while (true)`, so against a different
//! executable the injector thread spins forever. The game starts anyway, the
//! launcher logs "injected", and the player spends the evening on
//! FromSoftware's servers believing they are on ours. Refusing is the only
//! honest outcome.
//!
//! The "game already running" check is Linux-only and exists because Proton
//! takes an exclusive lock on the prefix with no timeout. A second launch
//! does not fail; it hangs with nothing on screen.

use std::path::PathBuf;

use ds2os_core::{exe, process, proton, GameInstall, GameType, ProtonBuild, Steam};

use crate::paths;
use crate::settings::Settings;

/// What the loader found, so a launch does not have to look it all up again.
pub struct Ready {
    pub install: GameInstall,
    pub game_exe: PathBuf,
    pub steam_root: PathBuf,
    #[allow(dead_code)] // Windows launches without Proton.
    pub proton: Option<ProtonBuild>,
    pub injector_source: PathBuf,
}

/// Why the loader will not start the game.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Problem {
    SteamMissing,
    SteamClosed,
    GameMissing,
    WrongBuild { size: u64, sha256: String },
    NoPrefix,
    NoProton,
    InjectorMissing,
    GameAlreadyRunning,
    NotWritable(PathBuf),
}

impl Problem {
    /// What the player reads. Says what is wrong and what to do about it.
    pub fn message(&self) -> String {
        match self {
            Problem::SteamMissing => {
                "não achei a Steam — aponte a pasta em configurações".to_owned()
            }
            Problem::SteamClosed => "abra a Steam e faça login antes de jogar".to_owned(),
            Problem::GameMissing => {
                "não achei o Dark Souls II — aponte a pasta em configurações".to_owned()
            }
            Problem::WrongBuild { size, .. } => format!(
                "este DarkSoulsII.exe não é a versão esperada ({size} bytes). \
                 Verifique os arquivos do jogo na Steam"
            ),
            Problem::NoPrefix => {
                "abra o jogo uma vez pela Steam para o Proton criar o prefixo".to_owned()
            }
            Problem::NoProton => "não achei nenhum Proton — instale um pela Steam".to_owned(),
            Problem::InjectorMissing => {
                "faltam os arquivos do injector — reinstale o loader".to_owned()
            }
            Problem::GameAlreadyRunning => "o jogo já está aberto".to_owned(),
            Problem::NotWritable(path) => {
                format!("não consigo escrever em {}", path.display())
            }
        }
    }
}

pub type Report = Result<Ready, Problem>;

/// Runs every check. Cheap except for the build fingerprint, which hashes
/// 28 MB, so callers on a UI thread should push this onto a worker.
pub fn run(settings: &Settings) -> Report {
    let steam = match &settings.steam_root {
        // An override that is not a Steam install is the player's mistake to
        // see, not something to fall through into a confusing "game missing".
        Some(root) if root.join("steamapps").is_dir() => Steam::from_root(root),
        Some(_) => return Err(Problem::SteamMissing),
        None => Steam::discover().map_err(|_| Problem::SteamMissing)?,
    };

    // `None` means the platform cannot tell, and a launch should go ahead on
    // it rather than blocking on a question we cannot answer.
    if process::steam_running() == Some(false) {
        return Err(Problem::SteamClosed);
    }

    let install = resolve_install(&steam, settings).ok_or(Problem::GameMissing)?;
    let game_exe = install.executable().ok_or(Problem::GameMissing)?;

    // Size first: a wrong file is usually wrong by size, and that costs a stat
    // instead of a hash.
    let size = exe::size_of(&game_exe).map_err(|_| Problem::GameMissing)?;
    if size != exe::DS2_SOTFS_1_03.size {
        return Err(Problem::WrongBuild {
            size,
            sha256: String::new(),
        });
    }
    let taken = exe::fingerprint(&game_exe).map_err(|_| Problem::GameMissing)?;
    if taken != exe::DS2_SOTFS_1_03 {
        return Err(Problem::WrongBuild {
            size: taken.size,
            sha256: taken.hex(),
        });
    }

    let proton = if cfg!(windows) {
        None
    } else {
        if install.prefix_path.is_none() {
            return Err(Problem::NoPrefix);
        }
        let build = match &settings.proton_dir {
            Some(dir) => proton::installed(&steam)
                .into_iter()
                .find(|build| &build.dir == dir),
            None => proton::for_game(&steam, &install),
        };
        Some(build.ok_or(Problem::NoProton)?)
    };

    let injector_source = find_injector().ok_or(Problem::InjectorMissing)?;

    // Only Linux: on Windows the game's own mutex refuses a second copy, and
    // there is no prefix lock to deadlock against.
    if cfg!(unix) && !process::game_processes(&["DarkSoulsII.exe"]).is_empty() {
        return Err(Problem::GameAlreadyRunning);
    }

    paths::ensure_dirs().map_err(|_| Problem::NotWritable(paths::data_dir()))?;

    Ok(Ready {
        install,
        game_exe,
        steam_root: steam.root().to_path_buf(),
        proton,
        injector_source,
    })
}

/// The game, from Steam or from an override the player typed in.
fn resolve_install(steam: &Steam, settings: &Settings) -> Option<GameInstall> {
    if let Some(dir) = &settings.game_dir {
        if !dir.is_dir() {
            return None;
        }
        // Believe the player about the folder, but keep looking for the prefix
        // in the usual place so Linux still works with an override.
        let app_id = GameType::DarkSouls2.app_id();
        let library = dir.parent().and_then(|p| p.parent()).and_then(|p| p.parent());
        let prefix = library.map(|lib| lib.join(format!("steamapps/compatdata/{app_id}/pfx")));

        return Some(GameInstall {
            app_id,
            game_type: GameType::DarkSouls2,
            install_dir: dir.clone(),
            library: library.map(Into::into).unwrap_or_else(|| dir.clone()),
            prefix_path: prefix.filter(|p| p.is_dir()),
        });
    }

    steam.find_game(GameType::DarkSouls2)
}

/// The first directory that has both injector binaries in it.
fn find_injector() -> Option<PathBuf> {
    let mut candidates = paths::injector_sources();
    // Already staged from a previous run counts too, so a loader whose sidecar
    // went missing still starts.
    candidates.push(paths::injector_dir());

    candidates.into_iter().find(|dir| {
        ["Injector.exe", "Injector.dll"]
            .iter()
            .all(|name| dir.join(name).is_file())
    })
}

/// Prints the whole report rather than only the first failure, for a player
/// who has been asked to paste it somewhere.
pub fn doctor(settings: &Settings) {
    match run(settings) {
        Ok(ready) => {
            println!("tudo pronto");
            println!("  jogo      {}", ready.game_exe.display());
            println!("  steam     {}", ready.steam_root.display());
            match &ready.install.prefix_path {
                Some(prefix) => println!("  prefixo   {}", prefix.display()),
                None => println!("  prefixo   nenhum"),
            }
            match &ready.proton {
                Some(build) => println!(
                    "  proton    {}  ({})",
                    build.dir.display(),
                    build.label.as_deref().unwrap_or("sem version")
                ),
                None => println!("  proton    não se aplica"),
            }
            println!("  injector  {}", ready.injector_source.display());
            println!("  dados     {}", paths::data_dir().display());
        }
        Err(problem) => {
            println!("bloqueado: {}", problem.message());
            if let Problem::WrongBuild { sha256, .. } = &problem {
                if !sha256.is_empty() {
                    println!("  sha256 encontrado {sha256}");
                    println!("  sha256 esperado   {}", exe::DS2_SOTFS_1_03.hex());
                }
            }
        }
    }
}
