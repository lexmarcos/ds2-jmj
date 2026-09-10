//! Is Steam up, and is the game already running.
//!
//! Both questions gate a launch. Steam has to be running because the server
//! validates Steam auth tickets, so a game started without it fails at login
//! with nothing on screen to explain why. And on Linux a second launch is
//! worse than a failure: Proton takes an exclusive lock on the prefix with no
//! timeout, so `proton run` against a prefix the running game already holds
//! blocks forever with no output at all.

/// Whether Steam is running, as far as the platform can honestly say.
///
/// `None` means "cannot tell" rather than "no", and callers should let a
/// launch proceed on it.
pub fn steam_running() -> Option<bool> {
    platform::steam_running()
}

/// Whether Steam has an account logged in.
///
/// Only Windows answers this. Linux Steam leaves nothing on disk that says so:
/// `registry.vdf` carries no active user and `loginusers.vdf` records who has
/// logged in before, not who is logged in now.
pub fn steam_logged_in() -> Option<bool> {
    platform::steam_logged_in()
}

/// Process ids of anything that looks like the game or its launcher.
///
/// Under Proton these are ordinary Linux processes whose command line names
/// the Windows executable, so that is what has to be matched — `/proc/<pid>/exe`
/// points at the Wine loader, not at the game.
///
/// The match is on a whole argument ending in the name, not on the name
/// appearing anywhere in the command line. The loose version finds a text
/// editor with the file open, a shell that merely mentions it, and this
/// process's own parent, and every one of those would wrongly disable the
/// button.
pub fn game_processes(exe_names: &[&str]) -> Vec<u32> {
    platform::game_processes(exe_names)
}

/// Whether a process id is still alive.
pub fn alive(pid: u32) -> bool {
    platform::alive(pid)
}

#[cfg(unix)]
mod platform {
    use std::path::PathBuf;

    pub fn steam_running() -> Option<bool> {
        let home = std::env::var("HOME").ok()?;
        // Steam writes this on start and leaves it behind on exit, so the pid
        // has to be checked rather than the file's existence.
        let pid_file = PathBuf::from(&home).join(".steam/steam.pid");
        let text = std::fs::read_to_string(&pid_file).ok()?;
        let pid: u32 = text.trim().parse().ok()?;

        if !alive(pid) {
            return Some(false);
        }

        // A recycled pid would otherwise read as Steam. Confirm the executable.
        let exe = std::fs::read_link(format!("/proc/{pid}/exe")).ok();
        let looks_like_steam = exe
            .map(|path| path.to_string_lossy().ends_with("/steam"))
            .unwrap_or(true);

        Some(looks_like_steam)
    }

    pub fn steam_logged_in() -> Option<bool> {
        None
    }

    pub fn alive(pid: u32) -> bool {
        std::path::Path::new(&format!("/proc/{pid}")).is_dir()
    }

    pub fn game_processes(exe_names: &[&str]) -> Vec<u32> {
        let Ok(entries) = std::fs::read_dir("/proc") else {
            return Vec::new();
        };

        let mut found = Vec::new();
        for entry in entries.flatten() {
            let name = entry.file_name();
            let Some(pid) = name.to_str().and_then(|text| text.parse::<u32>().ok()) else {
                continue;
            };

            if pid == std::process::id() {
                continue;
            }

            // NUL separated, so read it raw and split rather than treating the
            // whole thing as one string.
            let Ok(raw) = std::fs::read(format!("/proc/{pid}/cmdline")) else {
                continue;
            };
            let matches = raw.split(|byte| *byte == 0).any(|argument| {
                let argument = String::from_utf8_lossy(argument);
                let argument = argument.trim_end_matches(['"', '\'']);
                exe_names.iter().any(|needle| argument.ends_with(needle))
            });
            if matches {
                found.push(pid);
            }
        }

        found
    }
}

#[cfg(windows)]
mod platform {
    use winreg::enums::HKEY_CURRENT_USER;
    use winreg::RegKey;

    fn active_process() -> Option<RegKey> {
        RegKey::predef(HKEY_CURRENT_USER)
            .open_subkey(r"Software\Valve\Steam\ActiveProcess")
            .ok()
    }

    pub fn steam_running() -> Option<bool> {
        // Steam clears ActiveUser on logout and on exit, which makes it a
        // better signal than the pid: a pid can outlive a hung client.
        Some(steam_logged_in()? )
    }

    pub fn steam_logged_in() -> Option<bool> {
        let key = active_process()?;
        let user: u32 = key.get_value("ActiveUser").ok()?;
        Some(user != 0)
    }

    pub fn alive(_pid: u32) -> bool {
        // Windows does not need this: the game's own mutex refuses a second
        // copy, and there is no prefix lock to deadlock on.
        false
    }

    pub fn game_processes(_exe_names: &[&str]) -> Vec<u32> {
        Vec::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn this_process_is_alive() {
        let me = std::process::id();
        #[cfg(unix)]
        assert!(alive(me));
        let _ = me;
    }

    #[test]
    fn nothing_matches_an_impossible_name() {
        assert!(game_processes(&["definitely-not-a-real-executable-name.exe"]).is_empty());
    }

    /// A shell that merely mentions the game is not the game. This caught a
    /// real false positive: the scan matched the very command that launched
    /// it, because the name appeared inside a long `-c` argument.
    #[test]
    #[cfg(unix)]
    fn a_command_line_mentioning_the_game_is_not_the_game() {
        let mut child = std::process::Command::new("sh")
            .arg("-c")
            .arg("echo DarkSoulsII.exe > /dev/null; sleep 2")
            .spawn()
            .expect("spawn a shell");

        let found = game_processes(&["DarkSoulsII.exe"]);
        child.kill().ok();
        child.wait().ok();

        assert!(
            !found.contains(&child.id()),
            "matched a shell that only names the game: {found:?}"
        );
    }
}
