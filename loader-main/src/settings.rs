//! What the player can change.
//!
//! Every field here exists for one reason: it is the only way out of a check
//! the loader cannot fix by itself. Steam installed somewhere unusual, a game
//! folder moved by hand, a Proton the prefix does not name.
//!
//! Nothing about the server is here, and nothing about the patches. The
//! address and key are compiled in, and a settings entry would be a second
//! source of truth for them. The gameplay patches are the point of the
//! server, not a preference — and two of them cannot be separated anyway,
//! since the three area hooks install together behind one flag.

use std::path::PathBuf;

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(default, rename_all = "camelCase")]
pub struct Settings {
    /// Steam's root, when discovery finds nothing. A portable install, or a
    /// layout none of the usual paths cover.
    pub steam_root: Option<PathBuf>,

    /// The game folder, when Steam's manifest does not describe it — a
    /// library moved without Steam noticing, or a copy made by hand.
    pub game_dir: Option<PathBuf>,

    /// Which Proton to use. Linux only, and only when the prefix does not name
    /// one.
    pub proton_dir: Option<PathBuf>,
}

impl Settings {
    /// Reads settings, falling back to defaults for a missing or damaged file.
    ///
    /// A bad write must never be able to lock the player out of the loader,
    /// and defaults here mean "detect everything", which is the normal case.
    pub fn load() -> Self {
        std::fs::read_to_string(crate::paths::settings_path())
            .ok()
            .and_then(|text| serde_json::from_str(&text).ok())
            .unwrap_or_default()
    }

    pub fn save(&self) -> std::io::Result<()> {
        let path = crate::paths::settings_path();
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::write(path, serde_json::to_vec_pretty(self)?)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_detect_everything() {
        let settings = Settings::default();
        assert!(settings.steam_root.is_none());
        assert!(settings.game_dir.is_none());
        assert!(settings.proton_dir.is_none());
    }

    #[test]
    fn unknown_fields_do_not_break_loading() {
        // An older or newer loader's file must not lock anyone out.
        let text = r#"{"steamRoot":"/tmp/steam","somethingElse":42}"#;
        let settings: Settings = serde_json::from_str(text).unwrap();
        assert_eq!(settings.steam_root, Some(PathBuf::from("/tmp/steam")));
        assert!(settings.game_dir.is_none());
    }
}
