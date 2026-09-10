//! Resolving everything the harness needs, and reporting what is missing.
//!
//! Every command starts here, so a broken setup produces one clear list of
//! problems rather than a failure five steps later with no context.

use std::path::{Path, PathBuf};

use ds2os_core::steam::{GameDetection, GameInstall, GameType, Steam};
use serde::Serialize;

use crate::paths;
use crate::settings::HarnessConfig;

/// The DS3OS server binary and the data directory it writes beside itself.
#[derive(Debug, Clone, Serialize)]
pub struct ServerPaths {
    pub binary: PathBuf,
    pub working_dir: PathBuf,
    pub config: PathBuf,
    pub public_key: PathBuf,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Environment {
    pub repo_root: Option<PathBuf>,
    pub state_dir: PathBuf,
    pub steam_root: Option<PathBuf>,
    pub game: Option<GameDetection>,
    pub game_dir: Option<PathBuf>,
    pub game_exe: Option<PathBuf>,
    pub proton: Option<PathBuf>,
    pub server: Option<ServerPaths>,
    /// Directory holding the Windows-built Injector.dll and Injector.exe.
    pub injector_source: Option<PathBuf>,
    /// Every install the harness can prepare, one per Steam account.
    pub installs: Vec<Install>,
}

/// One game install: where it lives and which Steam account owns it.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Install {
    /// 1 is the account Steam runs by default, 2 the second client.
    pub account: u8,
    pub steam_root: PathBuf,
    pub game_dir: PathBuf,
    pub game_exe: Option<PathBuf>,
    pub prefix: Option<PathBuf>,
}

/// One thing that is wrong, and what to do about it.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Problem {
    pub what: String,
    pub fix: String,
}

impl Environment {
    /// Resolves everything, tolerating whatever is absent.
    pub fn resolve() -> Self {
        let repo_root = find_repo_root();
        let steam = Steam::discover().ok();

        let install: Option<GameInstall> =
            steam.as_ref().and_then(|s| s.find_game(GameType::DarkSouls2));
        let game = steam
            .as_ref()
            .map(|s| GameDetection::probe(s, GameType::DarkSouls2));

        let proton = steam.as_ref().and_then(|s| s.proton_builds().pop());

        let server = repo_root.as_ref().and_then(|root| {
            let working_dir = root.join("bin/x64_release/server");
            let binary = working_dir.join("Server");
            binary.is_file().then(|| ServerPaths {
                config: working_dir.join("Saved/default/config.json"),
                public_key: working_dir.join("Saved/default/public.key"),
                binary,
                working_dir,
            })
        });

        // Where the CI artifact gets unpacked comes first. The game directory
        // is only a fallback: it already holds a copy from a previous prepare,
        // and preferring it would mean a freshly downloaded build never gets
        // installed.
        let injector_source = [
            paths::home().join("Downloads/injector"),
            paths::home().join("injector"),
        ]
        .into_iter()
        .find(|dir| dir.join("Injector.dll").is_file())
        .or_else(|| {
            install
                .as_ref()
                .map(|i| i.install_dir.clone())
                .filter(|dir| dir.join("Injector.dll").is_file())
        });

        let mut installs = Vec::new();
        if let (Some(steam), Some(game)) = (steam.as_ref(), install.as_ref()) {
            installs.push(Install {
                account: 1,
                steam_root: steam.root().to_path_buf(),
                game_dir: game.install_dir.clone(),
                game_exe: game.executable(),
                prefix: game.prefix_path.clone(),
            });
        }
        // The second account has its own client, its own install and its own
        // Proton prefix, which is what keeps the single-instance mutex from
        // seeing the first one.
        if let Some(home) = HarnessConfig::load().second_steam_home {
            if let Ok(second) = Steam::discover_in(&home) {
                if let Some(game) = second.find_game(GameType::DarkSouls2) {
                    installs.push(Install {
                        account: 2,
                        steam_root: second.root().to_path_buf(),
                        game_exe: game.executable(),
                        prefix: game.prefix_path.clone(),
                        game_dir: game.install_dir,
                    });
                }
            }
        }

        Self {
            installs,
            repo_root,
            state_dir: paths::state_dir(),
            steam_root: steam.as_ref().map(|s| s.root().to_path_buf()),
            game_dir: install.as_ref().map(|i| i.install_dir.clone()),
            game_exe: install.as_ref().and_then(GameInstall::executable),
            game,
            proton,
            server,
            injector_source,
        }
    }

    /// Everything standing between the current state and a working test.
    pub fn problems(&self) -> Vec<Problem> {
        let mut problems = Vec::new();

        if self.steam_root.is_none() {
            problems.push(Problem {
                what: "Steam não encontrada".into(),
                fix: "instale a Steam, ou rode-a uma vez para criar ~/.steam".into(),
            });
        }
        if self.game_dir.is_none() {
            problems.push(Problem {
                what: "Dark Souls II não está instalado".into(),
                fix: "instale o Dark Souls II Scholar of the First Sin pela Steam".into(),
            });
        }
        if self.game.as_ref().and_then(|g| g.prefix_path.clone()).is_none() {
            problems.push(Problem {
                what: "o prefixo Proton do jogo não existe".into(),
                fix: "rode o jogo uma vez pela Steam para o Proton criar o prefixo".into(),
            });
        }
        if self.proton.is_none() {
            problems.push(Problem {
                what: "nenhum Proton instalado".into(),
                fix: "instale o Proton Experimental pela Steam".into(),
            });
        }
        if self.server.is_none() {
            problems.push(Problem {
                what: "o servidor não está compilado".into(),
                fix: "cd Tools && ./Build/cmake/linux/bin/cmake -S .. -B ../intermediate/make \
                      -G 'Unix Makefiles' -DCMAKE_BUILD_TYPE=Release && cd ../intermediate/make \
                      && make -j$(nproc) Server"
                    .into(),
            });
        }
        if self.injector_source.is_none() {
            problems.push(Problem {
                what: "Injector.dll não encontrado".into(),
                fix: "baixe o artefato 'injector' do workflow Injector for Linux e descompacte \
                      em ~/Downloads/injector"
                    .into(),
            });
        }

        problems
    }
}

/// Walks up from the executable and the working directory looking for the repo.
fn find_repo_root() -> Option<PathBuf> {
    let mut starts = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        starts.push(exe);
    }
    if let Ok(cwd) = std::env::current_dir() {
        starts.push(cwd);
    }

    for start in starts {
        let mut dir: Option<&Path> = Some(start.as_path());
        while let Some(current) = dir {
            if current.join("Source/Server.DarkSouls2").is_dir() {
                return Some(current.to_path_buf());
            }
            dir = current.parent();
        }
    }
    None
}
