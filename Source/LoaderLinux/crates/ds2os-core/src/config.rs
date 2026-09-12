//! The configuration file the loader owns.
//!
//! `InjectorConfig` is not ours to redesign: the injector reads it with
//! nlohmann/json straight into its `RuntimeConfig`, so the field names here must
//! match `Source/Injector/Config/RuntimeConfig.h` exactly. It has to sit beside
//! `Injector.dll` inside the Proton prefix, because the injector resolves it as
//! `<dll directory>/Injector.config`.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

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
    /// Reports the multiplay zone and its permissions.
    #[serde(default)]
    pub DS2ProbeMultiPlayZone: bool,
    /// Believe the player is always inside a multiplay zone.
    #[serde(default)]
    pub DS2ForceMultiPlayZone: bool,

    /// Build every fog wall as if the local player owned the world, which
    /// is what a phantom's area barrier appears to hang on.
    pub DS2RemovePhantomFog: bool,
    pub DS2AutoRematch: bool,
    /// Zone substituted when the game reports none.
    #[serde(default)]
    pub DS2ForcedZoneId: i32,
}

impl InjectorConfig {
    pub fn write_to(&self, dir: &Path) -> std::io::Result<PathBuf> {
        let path = dir.join(INJECTOR_CONFIG_FILE);
        std::fs::write(&path, serde_json::to_vec_pretty(self)?)?;
        Ok(path)
    }
}
