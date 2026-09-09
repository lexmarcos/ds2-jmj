//! Core logic for the ds2os Linux loader.
//!
//! Everything here is plain Rust with no Tauri dependency, so it can be tested
//! without a GUI toolchain and reused by a CLI later.

pub mod config;
pub mod launch;
pub mod master;
pub mod steam;
pub mod vdf;

pub use config::{InjectorConfig, LoaderSettings};
pub use master::{MasterClient, MasterError, ServerEntry};
pub use launch::{prepare, LaunchError, LaunchPlan};
pub use steam::{GameDetection, GameInstall, GameType, Steam, SteamError};
