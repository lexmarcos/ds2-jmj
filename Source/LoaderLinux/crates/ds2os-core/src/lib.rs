//! What the ds2os tools share: finding the game, reading Steam's files, and
//! writing the config the injector reads.
//!
//! Plain Rust with no GUI dependency. The Tauri loader that used to sit beside
//! this is gone, and with it went the master-server client and the Steam
//! launch-wrapper planner; nothing here talks to a server list any more.

pub mod config;
pub mod pem;
pub mod steam;
pub mod vdf;

pub use config::InjectorConfig;
pub use pem::normalize_public_key;
pub use steam::{GameDetection, GameInstall, GameType, Steam, SteamError};
