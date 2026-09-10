//! Where the loader keeps its own files.
//!
//! Not in the game folder. That is a Steam library: on Windows it can carry
//! awkward permissions, "verify integrity of game files" churns it, and every
//! hook writes its log next to the DLL — one of those reached 119 MB during
//! development. Somewhere the loader owns and can truncate is the only sane
//! place for that.

use std::path::PathBuf;

const APP: &str = "ds2os-loader";

/// Data the loader manages: the injector and its logs.
///
/// `~/.local/share/ds2os-loader` or `%LOCALAPPDATA%\ds2os-loader`.
pub fn data_dir() -> PathBuf {
    dirs::data_local_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join(APP)
}

/// The one directory `Injector.exe`, `Injector.dll` and `Injector.config`
/// share. They must be together: the launcher resolves the DLL beside itself,
/// and the DLL resolves its config beside itself.
pub fn injector_dir() -> PathBuf {
    data_dir().join("injector")
}

/// Where the loader's own logs go, separate from the injector's.
pub fn logs_dir() -> PathBuf {
    data_dir().join("logs")
}

/// Player settings, which are only ever overrides for a failed detection.
pub fn settings_path() -> PathBuf {
    dirs::config_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join(APP)
        .join("settings.json")
}

/// Directories the injector binaries might be sitting in, in the order they
/// should be preferred.
///
/// A release ships them beside the loader. A development build also looks in
/// the directory the CI artefact is unpacked into, so a freshly downloaded
/// injector is picked up without copying anything by hand.
pub fn injector_sources() -> Vec<PathBuf> {
    let mut sources = Vec::new();

    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            sources.push(dir.join("injector"));
        }
    }

    if cfg!(debug_assertions) {
        if let Some(home) = dirs::home_dir() {
            sources.push(home.join("Downloads/injector"));
        }
    }

    sources
}

pub fn ensure_dirs() -> std::io::Result<()> {
    std::fs::create_dir_all(injector_dir())?;
    std::fs::create_dir_all(logs_dir())?;
    if let Some(parent) = settings_path().parent() {
        std::fs::create_dir_all(parent)?;
    }
    Ok(())
}
