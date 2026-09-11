//! Reading what the injector says while the game starts.
//!
//! Its console output does not reach us: under Proton nothing the launcher
//! prints crosses back, and on Windows the console is suppressed. What it does
//! do is write files beside its DLL, which is why those live in a directory
//! the loader owns and clears before every run.
//!
//! The subtlety is which line means success. `injected, module handle` only
//! means the DLL was loaded. The hook that redirects the server searches for
//! the retail hostname inside a `while (true)`, so against an unexpected
//! executable it never returns and the game quietly runs on FromSoftware's
//! servers with the DLL sitting there. The honest signal is a later hook
//! reporting that it installed, because reaching it proves the earlier one
//! returned.

use std::collections::HashMap;
use std::path::PathBuf;

use crate::paths;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Signal {
    /// The game process exists.
    GameStarted,
    /// The DLL loaded. Not success on its own.
    DllLoaded,
    /// A later hook installed, which means the redirect hook returned.
    HooksInstalled,
    /// The game is running, but against the retail servers.
    NoInjector,
    /// The launcher could not start the game at all.
    CouldNotStart,
}

impl Signal {
    fn from_line(line: &str) -> Option<Self> {
        // Ordered so the decisive ones win when a line could match twice.
        if line.contains("abrindo areas para multiplayer")
            || line.contains("event=DS2ActiveTimerPatch result=installed")
        {
            Some(Signal::HooksInstalled)
        } else if line.contains("continuing without the injector") {
            Some(Signal::NoInjector)
        } else if line.contains("could not start the game")
            || line.contains("nothing to inject")
            || line.contains("refused to load the dll")
        {
            Some(Signal::CouldNotStart)
        } else if line.contains("injected, module handle") {
            Some(Signal::DllLoaded)
        } else if line.contains("started the game, pid") {
            Some(Signal::GameStarted)
        } else {
            None
        }
    }
}

/// Follows the injector's files, returning only what is new since last asked.
pub struct Watcher {
    offsets: HashMap<PathBuf, u64>,
}

impl Watcher {
    /// Starts from the current end of each file.
    ///
    /// The launch clears them first, so this is normally zero; starting from
    /// the end anyway means a file that somehow survived cannot replay an old
    /// session's "injected" as if it were this one's.
    pub fn new() -> Self {
        let mut offsets = HashMap::new();
        for path in Self::watched() {
            let end = std::fs::metadata(&path).map(|meta| meta.len()).unwrap_or(0);
            offsets.insert(path, end);
        }
        Self { offsets }
    }

    fn watched() -> Vec<PathBuf> {
        let dir = paths::injector_dir();
        vec![
            dir.join("DS2OS_Injector.log"),
            dir.join("DS2_UnlockAreas.log"),
            dir.join("DS2_TimerParamPatch.log"),
        ]
    }

    /// Everything the injector has said since the last call.
    pub fn poll(&mut self) -> Vec<Signal> {
        let mut signals = Vec::new();

        for path in Self::watched() {
            let Ok(bytes) = std::fs::read(&path) else {
                continue;
            };
            let seen = self.offsets.entry(path).or_insert(0);

            // A file that shrank was replaced; read it from the start.
            if (bytes.len() as u64) < *seen {
                *seen = 0;
            }

            let fresh = &bytes[(*seen as usize).min(bytes.len())..];
            *seen = bytes.len() as u64;

            for line in String::from_utf8_lossy(fresh).lines() {
                if let Some(signal) = Signal::from_line(line) {
                    signals.push(signal);
                }
            }
        }

        signals
    }
}

impl Default for Watcher {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_areas_banner_is_success() {
        assert_eq!(
            Signal::from_line("============ abrindo areas para multiplayer ============"),
            Some(Signal::HooksInstalled)
        );
    }

    #[test]
    fn the_timer_install_line_is_success() {
        assert_eq!(
            Signal::from_line("time=1.5 event=DS2ActiveTimerPatch result=installed reason=none"),
            Some(Signal::HooksInstalled)
        );
    }

    /// The line that reads like success and is not.
    #[test]
    fn a_loaded_dll_is_not_success() {
        assert_eq!(
            Signal::from_line("[injector] injected, module handle 0xfbe80000"),
            Some(Signal::DllLoaded)
        );
    }

    #[test]
    fn playing_on_retail_servers_is_a_failure() {
        assert_eq!(
            Signal::from_line(
                "[injector] continuing without the injector; the game will use the retail servers"
            ),
            Some(Signal::NoInjector)
        );
    }

    #[test]
    fn a_later_patch_line_is_not_the_install_line() {
        // The hook writes one of these per refill, thousands of times.
        assert_eq!(
            Signal::from_line("time=90.0 event=DS2ActiveTimerPatch result=patched source=r14"),
            None
        );
    }

    #[test]
    fn ordinary_lines_say_nothing() {
        assert_eq!(Signal::from_line("---- ds2os injector ----"), None);
        assert_eq!(Signal::from_line(""), None);
    }
}
