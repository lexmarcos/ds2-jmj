//! Settings the harness remembers between runs.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

use crate::paths;

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", default)]
pub struct HarnessConfig {
    /// Home directory of a second Steam client, logged into another account.
    ///
    /// The game's session layer is peer to peer over Steam and keyed on the
    /// account's steam id, so two instances can only reach each other if they
    /// are two accounts.
    pub second_steam_home: Option<PathBuf>,

    /// Which character each instance is supposed to load, by account.
    ///
    /// The game opens the last save played on that account, and one account
    /// here holds more than one character, so "it loaded" and "it loaded the
    /// right one" are different questions. With a name written down, the
    /// harness can answer the second from the server's own log.
    pub characters: std::collections::BTreeMap<u8, String>,
}

impl HarnessConfig {
    pub fn load() -> Self {
        std::fs::read_to_string(paths::harness_config())
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default()
    }

    pub fn save(&self) -> Result<(), String> {
        let path = paths::harness_config();
        let body = serde_json::to_vec_pretty(self).map_err(|e| e.to_string())?;
        std::fs::write(&path, body)
            .map_err(|e| format!("não consegui salvar {}: {e}", path.display()))
    }
}
