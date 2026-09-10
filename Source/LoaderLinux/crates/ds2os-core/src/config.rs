//! The two configuration files the loader owns.
//!
//! `InjectorConfig` is not ours to redesign: the injector reads it with
//! nlohmann/json straight into its `RuntimeConfig`, so the field names here must
//! match `Source/Injector/Config/RuntimeConfig.h` exactly. It has to sit beside
//! `Injector.dll` inside the Proton prefix, because the injector resolves it as
//! `<dll directory>/Injector.config`.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::steam::GameType;

pub const INJECTOR_CONFIG_FILE: &str = "Injector.config";

/// Mirrors the injector's RuntimeConfig. Field names are load bearing.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[allow(non_snake_case)]
pub struct InjectorConfig {
    pub ServerName: String,
    pub ServerHostname: String,
    pub ServerPublicKey: String,
    pub ServerGameType: String,
    pub ServerPort: i32,
    pub EnableSeperateSaveFiles: bool,
    pub DS2PatchPhantomTimers: bool,
    pub DS2PhantomTimerSeconds: f64,
    /// Exploratory probe that locates the current area id in memory.
    #[serde(default)]
    pub DS2ProbeArea: bool,
    /// With the probe on, watch reads of the address it finds.
    #[serde(default)]
    pub DS2WatchAreaReads: bool,
    /// Address of the area id, if already known; skips the scan.
    #[serde(default)]
    pub DS2AreaAddress: String,
}

impl InjectorConfig {
    pub fn write_to(&self, dir: &Path) -> std::io::Result<PathBuf> {
        let path = dir.join(INJECTOR_CONFIG_FILE);
        std::fs::write(&path, serde_json::to_vec_pretty(self)?)?;
        Ok(path)
    }
}

/// Settings the loader keeps for itself. Unlike InjectorConfig this is ours, so
/// it uses ordinary names and is renamed into the injector's shape on launch.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LoaderSettings {
    pub master_server_url: String,
    pub separate_saves: bool,
    /// Removes the roughly 12 minute PvP session limit. Dark Souls II only.
    pub patch_phantom_timers: bool,
    pub phantom_timer_seconds: f64,
    /// Directory holding the Windows-built Injector.dll and Injector.exe.
    /// Defaults to the directory the loader itself runs from.
    #[serde(default)]
    pub injector_dir: Option<PathBuf>,
}

impl Default for LoaderSettings {
    fn default() -> Self {
        Self {
            master_server_url: "http://ds3os-master.timleonard.uk:50020".to_owned(),
            separate_saves: true,
            patch_phantom_timers: false,
            phantom_timer_seconds: 4000.0,
            injector_dir: None,
        }
    }
}

impl LoaderSettings {
    /// Reads settings, falling back to defaults for a missing or corrupt file so
    /// a bad write can never lock the user out of the app.
    pub fn load(path: &Path) -> Self {
        std::fs::read_to_string(path)
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default()
    }

    pub fn save(&self, path: &Path) -> std::io::Result<()> {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::write(path, serde_json::to_vec_pretty(self)?)
    }

    /// Builds the injector config for one server.
    ///
    /// The phantom timer patch is forced off for anything that is not Dark Souls
    /// II, mirroring what the Windows loader does, so it can never reach a DS3
    /// session.
    pub fn to_injector_config(
        &self,
        game_type: GameType,
        server_name: &str,
        hostname: &str,
        port: i32,
        public_key: &str,
    ) -> InjectorConfig {
        let is_ds2 = matches!(game_type, GameType::DarkSouls2);

        InjectorConfig {
            ServerName: server_name.to_owned(),
            ServerHostname: hostname.to_owned(),
            ServerPublicKey: public_key.to_owned(),
            ServerGameType: format!("{game_type:?}"),
            ServerPort: port,
            EnableSeperateSaveFiles: self.separate_saves,
            DS2PatchPhantomTimers: is_ds2 && self.patch_phantom_timers,
            DS2PhantomTimerSeconds: self.phantom_timer_seconds,
            DS2ProbeArea: false,
            DS2WatchAreaReads: false,
            DS2AreaAddress: String::new(),
        }
    }
}

/// Servers the user typed in by hand, which the master server knows nothing
/// about.
pub fn manual_servers_path() -> PathBuf {
    settings_path().with_file_name("servers.json")
}

/// Where the loader keeps its own settings, following the XDG base directory
/// spec so it lands in ~/.config/ds2os/settings.json by default.
pub fn settings_path() -> PathBuf {
    let base = std::env::var("XDG_CONFIG_HOME")
        .ok()
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(std::env::var("HOME").unwrap_or_default()).join(".config"));

    base.join("ds2os/settings.json")
}
