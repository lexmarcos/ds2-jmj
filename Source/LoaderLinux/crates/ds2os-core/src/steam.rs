//! Locating Steam, its libraries, the installed games and their Proton prefixes.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::vdf;

/// Steam app ids for the games DS3OS supports.
pub const APPID_DARK_SOULS_2: u32 = 335300;
pub const APPID_DARK_SOULS_3: u32 = 374320;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum GameType {
    DarkSouls2,
    DarkSouls3,
}

impl GameType {
    pub fn app_id(self) -> u32 {
        match self {
            GameType::DarkSouls2 => APPID_DARK_SOULS_2,
            GameType::DarkSouls3 => APPID_DARK_SOULS_3,
        }
    }

    /// File name of the game executable inside its install directory.
    pub fn executable(self) -> &'static str {
        match self {
            GameType::DarkSouls2 => "DarkSoulsII.exe",
            GameType::DarkSouls3 => "DarkSoulsIII.exe",
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum SteamError {
    #[error("could not find a Steam installation; looked in {0}")]
    SteamNotFound(String),
    #[error("failed to read {path}: {source}")]
    Read { path: PathBuf, source: std::io::Error },
    #[error("failed to parse {path}: {message}")]
    Parse { path: PathBuf, message: String },
}

#[derive(Debug, Clone)]
pub struct Steam {
    root: PathBuf,
}

/// Where a game lives on disk, once found.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GameInstall {
    pub app_id: u32,
    pub game_type: GameType,
    /// Directory holding the game executable.
    pub install_dir: PathBuf,
    /// Proton prefix, present only once Steam has run the game at least once.
    pub prefix_path: Option<PathBuf>,
}

impl GameInstall {
    /// Full path to the game executable, if it can be found.
    ///
    /// Scholar of the First Sin keeps DarkSoulsII.exe under a `Game`
    /// subdirectory rather than at the root of the install, so this looks one
    /// level down rather than assuming either layout.
    pub fn executable(&self) -> Option<PathBuf> {
        let name = self.game_type.executable();

        let at_root = self.install_dir.join(name);
        if at_root.is_file() {
            return Some(at_root);
        }

        let entries = std::fs::read_dir(&self.install_dir).ok()?;
        entries
            .flatten()
            .map(|entry| entry.path().join(name))
            .find(|candidate| candidate.is_file())
    }

    /// `drive_c` inside the Proton prefix, where Windows-side files belong.
    pub fn drive_c(&self) -> Option<PathBuf> {
        self.prefix_path.as_ref().map(|prefix| prefix.join("drive_c"))
    }
}

impl Steam {
    /// Finds a Steam installation, preferring the native one over Flatpak.
    pub fn discover() -> Result<Self, SteamError> {
        Self::discover_in(&PathBuf::from(std::env::var("HOME").unwrap_or_default()))
    }

    /// Finds a Steam installation belonging to a particular home directory.
    ///
    /// A second Steam client, run with its own HOME, is a second logged-in
    /// account: separate credentials, separate steam id, and therefore a
    /// separate peer identity for the game's session layer.
    pub fn discover_in(home: &Path) -> Result<Self, SteamError> {
        let candidates = [
            home.join(".steam/root"),
            home.join(".steam/steam"),
            home.join(".steam/debian-installation"),
            home.join(".local/share/Steam"),
            home.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"),
        ];

        for candidate in &candidates {
            // `~/.steam/root` is a symlink, so resolve before testing.
            let resolved = candidate.canonicalize().unwrap_or_else(|_| candidate.clone());
            if resolved.join("steamapps").is_dir() {
                return Ok(Self { root: resolved });
            }
        }

        let looked = candidates
            .iter()
            .map(|path| path.display().to_string())
            .collect::<Vec<_>>()
            .join(", ");
        Err(SteamError::SteamNotFound(looked))
    }

    pub fn from_root(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into() }
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    /// Every Steam library on this machine, starting with the main one.
    ///
    /// A missing or unparsable `libraryfolders.vdf` is not fatal: the install
    /// root is always a library, so we fall back to just that.
    pub fn libraries(&self) -> Vec<PathBuf> {
        let mut libraries = vec![self.root.clone()];

        let manifest = self.root.join("steamapps/libraryfolders.vdf");
        let Ok(text) = std::fs::read_to_string(&manifest) else {
            return libraries;
        };
        let Ok(parsed) = vdf::parse(&text) else {
            return libraries;
        };
        let Some(folders) = parsed.get("libraryfolders") else {
            return libraries;
        };

        for (_, entry) in folders.entries() {
            let Some(path) = entry.get("path").and_then(vdf::Value::as_str) else {
                continue;
            };
            let path = PathBuf::from(path);
            if !libraries.contains(&path) {
                libraries.push(path);
            }
        }

        libraries
    }

    /// Finds an installed game, if Steam has it on any library.
    pub fn find_game(&self, game_type: GameType) -> Option<GameInstall> {
        let app_id = game_type.app_id();

        for library in self.libraries() {
            let manifest = library.join(format!("steamapps/appmanifest_{app_id}.acf"));
            let Ok(text) = std::fs::read_to_string(&manifest) else {
                continue;
            };
            let Ok(parsed) = vdf::parse(&text) else {
                continue;
            };
            let Some(install_name) = parsed
                .path(["AppState", "installdir"])
                .and_then(vdf::Value::as_str)
            else {
                continue;
            };

            let install_dir = library.join("steamapps/common").join(install_name);
            if !install_dir.is_dir() {
                continue;
            }

            let prefix = library.join(format!("steamapps/compatdata/{app_id}/pfx"));
            return Some(GameInstall {
                app_id,
                game_type,
                install_dir,
                prefix_path: prefix.is_dir().then_some(prefix),
            });
        }

        None
    }

    /// Proton builds Steam knows about, official and third party.
    pub fn proton_builds(&self) -> Vec<PathBuf> {
        let mut builds = Vec::new();

        for library in self.libraries() {
            collect_dirs(&library.join("steamapps/common"), "Proton", &mut builds);
        }
        // Third party builds (GE-Proton and friends) live outside the libraries.
        collect_dirs(&self.root.join("compatibilitytools.d"), "", &mut builds);

        builds.sort();
        builds
    }
}

/// Collects directories under `parent` whose name starts with `prefix` and that
/// look like a Proton build.
fn collect_dirs(parent: &Path, prefix: &str, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(parent) else {
        return;
    };

    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if !name.starts_with(prefix) {
            continue;
        }
        // A Proton build always ships the `proton` launcher script.
        if path.join("proton").is_file() && !out.contains(&path) {
            out.push(path);
        }
    }
}

/// What the UI needs to know about one game. This is the shape the frontend
/// consumes, so "not installed" is expressed as null fields rather than as an
/// absent object.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct GameDetection {
    pub app_id: u32,
    pub game_type: GameType,
    pub install_dir: Option<PathBuf>,
    pub prefix_path: Option<PathBuf>,
    pub proton_name: Option<String>,
}

impl GameDetection {
    /// Reports what Steam has for `game_type`, including the "nothing" case.
    pub fn probe(steam: &Steam, game_type: GameType) -> Self {
        let install = steam.find_game(game_type);
        let proton_name = steam
            .proton_builds()
            .last()
            .and_then(|path| path.file_name())
            .map(|name| name.to_string_lossy().into_owned());

        Self {
            app_id: game_type.app_id(),
            game_type,
            install_dir: install.as_ref().map(|game| game.install_dir.clone()),
            prefix_path: install.and_then(|game| game.prefix_path),
            proton_name,
        }
    }
}
