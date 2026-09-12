//! Starting long lived processes and telling later whether they are still up.

use std::fs::File;
use std::path::Path;
use std::process::{Command, Stdio};

/// A process the harness started.
pub struct Managed {
    pub pid: u32,
}

/// Reads a pidfile and reports the pid only if that process is still alive and
/// still looks like what we started, so a recycled pid is never mistaken for
/// our process.
pub fn running(pid_file: &Path, expect_in_cmdline: &str) -> Option<u32> {
    let pid: u32 = std::fs::read_to_string(pid_file).ok()?.trim().parse().ok()?;
    let cmdline = std::fs::read(format!("/proc/{pid}/cmdline")).ok()?;
    let cmdline = String::from_utf8_lossy(&cmdline);
    cmdline.contains(expect_in_cmdline).then_some(pid)
}

/// Starts a process detached, with stdout and stderr appended to `log`.
///
/// `unbuffered` runs the command through stdbuf. The DS3OS server writes with
/// stdio, which block-buffers as soon as it is piped to a file: without this
/// the log looks frozen minutes behind reality, which is a genuinely confusing
/// way to debug a server.
pub fn spawn(
    program: &Path,
    args: &[&str],
    working_dir: &Path,
    env: &[(&str, String)],
    log: &Path,
    pid_file: &Path,
    unbuffered: bool,
) -> std::io::Result<Managed> {
    if let Some(parent) = log.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let out = File::create(log)?;
    let err = out.try_clone()?;

    let mut command = if unbuffered {
        let mut c = Command::new("stdbuf");
        c.arg("-oL").arg("-eL").arg(program);
        c
    } else {
        Command::new(program)
    };

    command
        .args(args)
        .current_dir(working_dir)
        .stdin(Stdio::null())
        .stdout(out)
        .stderr(err);

    for (key, value) in env {
        command.env(key, value);
    }

    let child = command.spawn()?;
    std::fs::write(pid_file, child.id().to_string())?;

    Ok(Managed { pid: child.id() })
}

/// Asks a process to stop, then waits briefly for it to actually go.
pub fn stop(pid: u32) -> bool {
    unsafe {
        libc_kill(pid as i32, 15);
    }
    for _ in 0..50 {
        if std::fs::metadata(format!("/proc/{pid}")).is_err() {
            return true;
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
    unsafe {
        libc_kill(pid as i32, 9);
    }
    std::thread::sleep(std::time::Duration::from_millis(200));
    std::fs::metadata(format!("/proc/{pid}")).is_err()
}

// Signalling is the one thing here that needs libc, and pulling in the crate
// for a single call is not worth it.
extern "C" {
    #[link_name = "kill"]
    fn libc_kill(pid: i32, sig: i32) -> i32;
}

/// Every pid actually running `needle`, by looking at whole arguments rather
/// than at the command line as one string.
///
/// A substring search over the whole command line matches anything that merely
/// *mentions* the executable: a `grep DarkSoulsII.exe`, an editor, another
/// agent's shell. The harness then reports instances that do not exist, and
/// "is the game already running" — the check that keeps a second launch from
/// hanging forever on the prefix lock — answers yes when nothing is running.
/// So an argument only counts when it ends in the name **and** carries a path
/// separator, which a command line's own mention of the file never does.
pub fn pids_matching(needle: &str) -> Vec<u32> {
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return Vec::new();
    };

    let mut found = Vec::new();
    for entry in entries.flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        if pid == std::process::id() {
            continue;
        }
        let Ok(cmdline) = std::fs::read(format!("/proc/{pid}/cmdline")) else {
            continue;
        };
        let runs_it = cmdline.split(|byte| *byte == 0).any(|argument| {
            let argument = String::from_utf8_lossy(argument);
            let argument = argument.trim_matches(['"', '\'']);
            argument.ends_with(needle) && argument.contains(['/', '\\'])
        });
        if runs_it {
            found.push(pid);
        }
    }
    found.sort_unstable();
    found
}

/// One variable from a process's environment, when it is readable.
pub fn env_of(pid: u32, key: &str) -> Option<String> {
    let raw = std::fs::read(format!("/proc/{pid}/environ")).ok()?;
    let prefix = format!("{key}=");
    raw.split(|byte| *byte == 0)
        .map(|entry| String::from_utf8_lossy(entry).into_owned())
        .find_map(|entry| entry.strip_prefix(&prefix).map(str::to_owned))
}

/// Waits for every one of `pids` to disappear.
///
/// Relaunching into a Proton prefix while anything still holds it means the new
/// `proton run` blocks on `pfx.lock`, which has no timeout: the harness looks
/// hung and the only way out is killing it by hand. Nothing may start until the
/// last process is gone.
pub fn wait_gone(pids: &[u32], timeout: std::time::Duration) -> bool {
    let deadline = std::time::Instant::now() + timeout;
    loop {
        let alive: Vec<u32> = pids
            .iter()
            .copied()
            .filter(|pid| std::fs::metadata(format!("/proc/{pid}")).is_ok())
            .collect();
        if alive.is_empty() {
            return true;
        }
        if std::time::Instant::now() >= deadline {
            return false;
        }
        std::thread::sleep(std::time::Duration::from_millis(200));
    }
}

/// Whether anything is listening on a local TCP port.
pub fn tcp_port_busy(port: u16) -> bool {
    std::net::TcpStream::connect_timeout(
        &std::net::SocketAddr::from(([127, 0, 0, 1], port)),
        std::time::Duration::from_millis(300),
    )
    .is_ok()
}
