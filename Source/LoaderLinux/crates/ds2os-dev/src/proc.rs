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

/// Every pid whose command line contains `needle`. Used to notice the
/// Steam-launched instance, which the harness does not start itself.
pub fn pids_matching(needle: &str) -> Vec<u32> {
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return Vec::new();
    };

    let mut found = Vec::new();
    for entry in entries.flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else {
            continue;
        };
        let Ok(cmdline) = std::fs::read(format!("/proc/{pid}/cmdline")) else {
            continue;
        };
        if String::from_utf8_lossy(&cmdline).contains(needle) {
            found.push(pid);
        }
    }
    found.sort_unstable();
    found
}

/// Whether anything is listening on a local TCP port.
pub fn tcp_port_busy(port: u16) -> bool {
    std::net::TcpStream::connect_timeout(
        &std::net::SocketAddr::from(([127, 0, 0, 1], port)),
        std::time::Duration::from_millis(300),
    )
    .is_ok()
}
