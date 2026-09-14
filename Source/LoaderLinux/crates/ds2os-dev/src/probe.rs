//! Asking the game itself where it is.
//!
//! Everything else the harness can see is second hand: the server only knows
//! about a client that is talking to it, and a screenshot needs someone to look
//! at it. But the injector is inside the process and answers request files, so
//! one byte of the game's own memory settles the question the driver keeps
//! asking — title screen, or world?
//!
//! `0x141614804` is set to 1 while the title screen's state machine is alive
//! and cleared when it tears down, which is the moment loading begins. Measured
//! both ways on 1.03 / Calibrations 2.02: 1 at "PRESS START BUTTON", 0 standing
//! in Majula. Like every other address here it is version specific; a wrong
//! answer must therefore be treated as "unknown", never as "in the world".

use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use ds2os_core::exe::Fingerprint;
use serde::Serialize;

use crate::env::Install;
use crate::observe::Build;

const REQUEST: &str = "DS2_MemProbe.req";
const ANSWER: &str = "DS2_MemProbe.log";
const LOCK: &str = "DS2_MemProbe.lock";

/// Module relative address of the title state's "I am alive" byte.
const TITLE_FLAG: &str = "1614804";

/// Where the game is, as far as the injector can say.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Where {
    Title,
    Loading,
    World,
    /// No answer: the game may still be booting, or the injector is not in it.
    Unknown,
}

/// Why `locate` answered what it did. The names are part of the JSON contract
/// (the `locate` event, `observe`'s `stateReason`, `doctor`), so they only grow.
///
/// Before these existed every failure was the same `unknown`, and `game enter`
/// ended in a bare timeout. From 13/09 the build check looked for the
/// executable in the wrong directory, so no request was ever written and no
/// button ever pressed, and nothing in the run said so.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Reason {
    /// Title: the title state's byte reads 1.
    TitleFlagSet,
    /// World: the byte reads 0 and the published pose is advancing.
    TelemetryAdvancing,
    /// Loading: the byte reads 0 and no pose is being published.
    NoTelemetry,
    /// No game process in the instance's prefix, so there is nobody to ask.
    InstanceStopped,
    /// No installation resolved for the account.
    InstanceMissing,
    /// The installation resolved no executable, or it cannot be read.
    ExeMissing,
    /// The executable is not the build the address was measured against, so
    /// the request is never written.
    UnsupportedExe,
    /// Another reader held the installation's probe lock for the whole budget.
    ProbeBusy,
    /// The request file, or its lock, could not be written.
    RequestWriteFailed,
    /// The request is still on disk: nothing in the game is reading requests.
    /// The game is still booting, or runs an injector without the probe.
    RequestNotConsumed,
    /// The request was taken, but no reply for its label turned up in time.
    NoAnswer,
    /// A reply for the label that does not have the shape of a byte dump.
    MalformedAnswer,
    /// The injector answered that the address cannot be read.
    Unreadable,
    /// The injector answered a value other than 0 or 1.
    UnexpectedByte,
    /// The byte reads 0 and a pose is published, but its tick never moved.
    TelemetryStalled,
    /// `observe`: no fresh sample from the same process and boot.
    StaleTelemetry,
    /// The caller's deadline had already passed.
    Timeout,
    /// The operation was cancelled.
    Cancelled,
}

/// The serialized name, which is also what people read in errors.
fn name(value: &impl Serialize) -> String {
    serde_json::to_value(value).ok().and_then(|v| v.as_str().map(str::to_owned)).unwrap_or_default()
}
impl std::fmt::Display for Where {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(&name(self)) }
}
impl std::fmt::Display for Reason {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(&name(self)) }
}

/// One answer from `locate`, with the evidence behind it. Every one of them is
/// also recorded as a `locate` event in the run.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Located {
    pub instance: u8,
    pub state: Where,
    pub reason: Reason,
    /// The executable whose build was checked.
    pub exe: Option<PathBuf>,
    pub label: Option<String>,
    pub request_written: bool,
    pub byte: Option<u8>,
    pub tick_before: Option<u64>,
    pub tick_after: Option<u64>,
    pub elapsed_ms: u128,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
}

impl Located {
    fn new(instance: u8, exe: Option<PathBuf>) -> Self {
        Self { instance, state: Where::Unknown, reason: Reason::Timeout, exe, label: None, request_written: false,
            byte: None, tick_before: None, tick_after: None, elapsed_ms: 0, detail: None }
    }

    /// An `unknown` decided before the game could be asked.
    pub fn unasked(instance: u8, exe: Option<PathBuf>, reason: Reason) -> Self {
        let found = Self { reason, ..Self::new(instance, exe) };
        found.record();
        found
    }

    fn record(&self) { crate::output::event("locate", serde_json::json!(self)); }
}

/// Reads the title flag out of one instance.
///
/// The injector polls for the request file twice a second, so an answer takes
/// about that long. Anything slower than `timeout` counts as unknown rather
/// than as an error: this is asked in a loop, and a driver that stops because
/// one read was slow is worse than one that tries again.
///
/// The request files live beside the injector, in the install root; the
/// executable whose build is checked first lives wherever the install says.
pub fn locate(install: &Install, timeout: Duration) -> Located {
    locate_build(install, ds2os_core::exe::DS2_SOTFS_1_03, timeout)
}

fn locate_build(install: &Install, expected: Fingerprint, timeout: Duration) -> Located {
    let started = Instant::now();
    let mut found = Located::new(install.account, install.game_exe.clone());
    (found.state, found.reason) = ask(install, expected, started + timeout, &mut found);
    found.elapsed_ms = started.elapsed().as_millis();
    found.record();
    found
}

fn ask(install: &Install, expected: Fingerprint, deadline: Instant, found: &mut Located) -> (Where, Reason) {
    use Reason::*;
    const UNKNOWN: Where = Where::Unknown;
    match crate::observe::build_of(install, expected) {
        Build::Expected => {}
        Build::Missing => return (UNKNOWN, ExeMissing),
        Build::Other => return (UNKNOWN, UnsupportedExe),
    }
    let dir = install.game_dir.as_path();
    // `observe` and `doctor` run beside a controller, so a held lock is waited
    // for rather than taken as an answer.
    let _lock = loop {
        match crate::control::Lock::acquire(&dir.join(LOCK)) {
            Ok(lock) => break lock,
            Err(e) if !e.starts_with("busy:") => { found.detail = Some(e); return (UNKNOWN, RequestWriteFailed); }
            Err(_) if Instant::now() >= deadline => return (UNKNOWN, ProbeBusy),
            Err(_) => if crate::control::sleep(Duration::from_millis(50)).is_err() { return (UNKNOWN, Cancelled); },
        }
    };
    let answer = dir.join(ANSWER);
    // Only what is appended after the request can answer it, so the log, which
    // grows by megabytes over a day, is not read whole on every poll.
    let from = std::fs::metadata(&answer).map(|m| m.len()).unwrap_or(0);
    // The existing injector echoes labels. A unique label correlates requests
    // without truncating another reader's reply or requiring a DLL upgrade.
    let label = format!("titleflag-{}", crate::output::id());
    found.label = Some(label.clone());
    if let Err(e) = write_request(dir, &format!("mod {label} {TITLE_FLAG} 1\n")) {
        found.detail = Some(e.to_string());
        return (UNKNOWN, RequestWriteFailed);
    }
    found.request_written = true;

    let reply = loop {
        if crate::control::check().is_err() { return (UNKNOWN, Cancelled); }
        if let Some(reply) = read_reply(&answer, &label, from) { break reply; }
        if Instant::now() >= deadline {
            return (UNKNOWN, if dir.join(REQUEST).exists() { RequestNotConsumed } else { NoAnswer });
        }
        if crate::control::sleep(Duration::from_millis(100)).is_err() { return (UNKNOWN, Cancelled); }
    };
    let byte = match reply {
        Reply::Byte(byte) => byte,
        Reply::Unreadable => return (UNKNOWN, Unreadable),
        Reply::Malformed => return (UNKNOWN, MalformedAnswer),
    };
    found.byte = Some(byte);
    match byte {
        1 => (Where::Title, TitleFlagSet),
        0 => {
            let Some(before) = crate::nav::read(dir) else { return (Where::Loading, NoTelemetry); };
            found.tick_before = Some(before.tick);
            loop {
                if Instant::now() >= deadline { return (UNKNOWN, TelemetryStalled); }
                if crate::control::sleep(Duration::from_millis(60)).is_err() { return (UNKNOWN, Cancelled); }
                let Some(now) = crate::nav::read(dir) else { return (Where::Loading, NoTelemetry); };
                found.tick_after = Some(now.tick);
                if now.tick > before.tick { return (Where::World, TelemetryAdvancing); }
            }
        }
        _ => (UNKNOWN, UnexpectedByte),
    }
}

/// Writes the request under a temporary name and moves it into place, so the
/// injector never reads half a file.
fn write_request(install_dir: &Path, body: &str) -> std::io::Result<()> {
    let temporary: PathBuf = install_dir.join("DS2_MemProbe.req.tmp");
    std::fs::write(&temporary, body)?;
    std::fs::rename(&temporary, install_dir.join(REQUEST))
}

#[derive(Debug, PartialEq, Eq)]
enum Reply { Byte(u8), Unreadable, Malformed }

/// The first byte reported for `label` after byte `from` of the log, from a
/// reply that looks like
///
/// ```text
/// === pedido titleflag ===
///   titleflag: 0x0000000141614804 (1 bytes)
///     0000000141614804 +0x000 01
/// ```
///
/// or `  titleflag: 0x0000000141614804 ilegivel` when the address cannot be
/// read. A line without its newline is still being written and does not count.
fn read_reply(answer: &Path, label: &str, from: u64) -> Option<Reply> {
    use std::io::{Read, Seek, SeekFrom};
    let mut file = std::fs::File::open(answer).ok()?;
    // A log that shrank was replaced; its whole content is new.
    let start = if file.metadata().ok()?.len() < from { 0 } else { from };
    file.seek(SeekFrom::Start(start)).ok()?;
    let mut raw = Vec::new();
    file.read_to_end(&mut raw).ok()?;
    let text = String::from_utf8_lossy(&raw);
    let head = format!("{label}:");
    let mut lines = text.split_inclusive('\n');
    while let Some(line) = lines.next() {
        let Some(rest) = line.trim_start().strip_prefix(head.as_str()) else { continue };
        if !line.ends_with('\n') { return None; }
        let rest = rest.trim();
        if rest.ends_with("ilegivel") || rest == "nao resolvido" { return Some(Reply::Unreadable); }
        let values = lines.next().filter(|v| v.ends_with('\n'))?;
        return Some(values.split_whitespace().last().and_then(|v| u8::from_str_radix(v, 16).ok())
            .map_or(Reply::Malformed, Reply::Byte));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{atomic::{AtomicBool, Ordering}, Arc};

    #[test]
    fn reads_the_byte_out_of_a_reply() {
        let dir = std::env::temp_dir().join(format!("ds2os-probe-{}", crate::output::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let answer = dir.join("reply.log");
        std::fs::write(
            &answer,
            "=== pedido titleflag ===\n  titleflag: 0x0000000141614804 (1 bytes)\n    \
             0000000141614804 +0x000 01\n",
        )
        .unwrap();
        assert_eq!(read_reply(&answer, "titleflag", 0), Some(Reply::Byte(1)));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn an_answer_for_another_label_is_not_ours() {
        let dir = std::env::temp_dir().join(format!("ds2os-probe-other-{}", crate::output::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let answer = dir.join("reply.log");
        std::fs::write(
            &answer,
            "  areaid: 0x0000000141614800 (4 bytes)\n    0000000141614800 +0x000 00 00 00 00\n",
        )
        .unwrap();
        assert_eq!(read_reply(&answer, "titleflag", 0), None);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn old_reply_cannot_satisfy_a_new_request_id() {
        let path = std::env::temp_dir().join(format!("ds2-reply-{}", crate::output::id()));
        std::fs::write(&path, "titleflag-old: address\n address +0x000 00\n").unwrap();
        assert_eq!(read_reply(&path, "titleflag-new", 0), None);
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn only_what_follows_the_request_and_is_complete_counts() {
        let path = std::env::temp_dir().join(format!("ds2-reply-partial-{}", crate::output::id()));
        let old = "  titleflag-1: 0x0000000141614804 (1 bytes)\n    0000000141614804 +0x000 01\n";
        std::fs::write(&path, old).unwrap();
        assert_eq!(read_reply(&path, "titleflag-1", old.len() as u64), None);
        // A log replaced by a shorter one is read from its start.
        std::fs::write(&path, "  titleflag-1: 0x0000000141614804 (1 bytes)\n    00 +0x000 00\n").unwrap();
        assert_eq!(read_reply(&path, "titleflag-1", 1 << 20), Some(Reply::Byte(0)));
        // The dump line has not arrived, or not all of it.
        std::fs::write(&path, "  titleflag-2: 0x0000000141614804 (1 bytes)\n    0000000141614804 +0x000 0").unwrap();
        assert_eq!(read_reply(&path, "titleflag-2", 0), None);
        std::fs::write(&path, "  titleflag-3: 0x0000000141614804 ilegivel\n").unwrap();
        assert_eq!(read_reply(&path, "titleflag-3", 0), Some(Reply::Unreadable));
        std::fs::write(&path, "  titleflag-4: 0x0000000141614804 (1 bytes)\n    ===\n").unwrap();
        assert_eq!(read_reply(&path, "titleflag-4", 0), Some(Reply::Malformed));
        std::fs::remove_file(path).unwrap();
    }

    /// A Scholar of the First Sin layout in a temporary directory, with a
    /// stand-in executable.
    struct FakeInstall { root: PathBuf, install: Install, build: Fingerprint, stop: Arc<AtomicBool> }

    impl FakeInstall {
        fn new() -> Self {
            let root = std::env::temp_dir().join(format!("ds2os-fake-install-{}", crate::output::id()));
            std::fs::create_dir_all(root.join("Game")).unwrap();
            let exe = root.join("Game").join("DarkSoulsII.exe");
            std::fs::write(&exe, b"not the game").unwrap();
            let build = ds2os_core::exe::fingerprint(&exe).unwrap();
            let install = Install { account: 1, steam_root: root.clone(), game_dir: root.clone(),
                game_exe: Some(exe), prefix: None };
            Self { root, install, build, stop: Arc::new(AtomicBool::new(false)) }
        }

        /// Answers requests the way `DS2_MemProbeHook` does: takes the file,
        /// removes it, and appends a dump for each `mod` line.
        fn injector(&self, reply: impl Fn(&str) -> String + Send + 'static) {
            let (root, stop) = (self.root.clone(), self.stop.clone());
            std::thread::spawn(move || while !stop.load(Ordering::Relaxed) {
                let request = root.join(REQUEST);
                if let Ok(body) = std::fs::read_to_string(&request) {
                    std::fs::remove_file(&request).unwrap();
                    use std::io::Write;
                    let mut log = std::fs::OpenOptions::new().create(true).append(true).open(root.join(ANSWER)).unwrap();
                    for line in body.lines() {
                        let label = line.split_whitespace().nth(1).unwrap();
                        write!(log, "\n=== pedido {label} ===\n{}", reply(label)).unwrap();
                    }
                }
                std::thread::sleep(Duration::from_millis(20));
            });
        }

        /// Publishes a pose whose tick advances, like `DS2_NavHook` in a world.
        fn walking(&self) {
            let (root, stop) = (self.root.clone(), self.stop.clone());
            std::thread::spawn(move || {
                let mut tick = 100;
                while !stop.load(Ordering::Relaxed) {
                    tick += 1;
                    let pose = format!("6.1855 -18.5166 209.0531 -0.0000 -1.0000 00007FFFF03A6690 {tick} 6 boot-1\n");
                    std::fs::write(root.join("DS2_Nav.tmp"), pose).unwrap();
                    std::fs::rename(root.join("DS2_Nav.tmp"), root.join("DS2_Nav.txt")).unwrap();
                    std::thread::sleep(Duration::from_millis(30));
                }
            });
        }

        fn locate(&self, timeout: Duration) -> Located { locate_build(&self.install, self.build, timeout) }
    }

    impl Drop for FakeInstall {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::Relaxed);
            std::thread::sleep(Duration::from_millis(60));
            std::fs::remove_dir_all(&self.root).ok();
        }
    }

    fn dump(label: &str, byte: u8) -> String {
        format!("  {label}: 0x0000000141614804 (1 bytes)\n    0000000141614804 +0x000 {byte:02x}\n")
    }

    #[test]
    fn the_title_is_read_from_the_install_the_harness_resolved() {
        let fake = FakeInstall::new();
        fake.injector(|label| dump(label, 1));
        let found = fake.locate(Duration::from_secs(3));
        assert_eq!((found.state, found.reason), (Where::Title, Reason::TitleFlagSet));
        assert!(found.request_written);
        assert_eq!(found.byte, Some(1));
    }

    #[test]
    fn another_build_is_never_asked() {
        let fake = FakeInstall::new();
        let found = locate_build(&fake.install, ds2os_core::exe::DS2_SOTFS_1_03, Duration::from_millis(300));
        assert_eq!((found.state, found.reason), (Where::Unknown, Reason::UnsupportedExe));
        assert!(!found.request_written);
        assert!(!fake.root.join(REQUEST).exists());

        let gone = Install { game_exe: Some(fake.root.join("DarkSoulsII.exe")), ..fake.install.clone() };
        let found = locate_build(&gone, fake.build, Duration::from_millis(300));
        assert_eq!(found.reason, Reason::ExeMissing);
        assert!(!fake.root.join(REQUEST).exists());
    }

    #[test]
    fn a_request_nobody_takes_is_told_apart_from_one_nobody_answers() {
        let fake = FakeInstall::new();
        let found = fake.locate(Duration::from_millis(400));
        assert_eq!((found.state, found.reason), (Where::Unknown, Reason::RequestNotConsumed));
        assert!(found.request_written);

        fake.injector(|_| String::new());
        let found = fake.locate(Duration::from_millis(600));
        assert_eq!((found.state, found.reason), (Where::Unknown, Reason::NoAnswer));
    }

    #[test]
    fn a_cleared_flag_is_world_only_while_the_pose_advances() {
        let fake = FakeInstall::new();
        fake.injector(|label| dump(label, 0));
        let found = fake.locate(Duration::from_secs(3));
        assert_eq!((found.state, found.reason), (Where::Loading, Reason::NoTelemetry));

        // A pose that stays put is a frozen game, not a world.
        std::fs::write(fake.root.join("DS2_Nav.txt"), "1 2 3 0 1 00007FFFF03A6690 7 6 boot-1\n").unwrap();
        // Well inside the two seconds after which `nav::read` stops believing the file.
        let found = fake.locate(Duration::from_millis(1000));
        assert_eq!((found.state, found.reason), (Where::Unknown, Reason::TelemetryStalled));
        assert_eq!(found.tick_before, Some(7));

        fake.walking();
        let found = fake.locate(Duration::from_secs(3));
        assert_eq!((found.state, found.reason), (Where::World, Reason::TelemetryAdvancing));
        assert!(found.tick_after > found.tick_before);
    }

    #[test]
    fn a_held_probe_lock_is_waited_for_and_then_reported() {
        let fake = FakeInstall::new();
        fake.injector(|label| dump(label, 1));
        let held = crate::control::Lock::acquire(&fake.root.join(LOCK)).unwrap();
        let found = fake.locate(Duration::from_millis(300));
        assert_eq!((found.state, found.reason), (Where::Unknown, Reason::ProbeBusy));
        assert!(!found.request_written);

        let release = std::thread::spawn(move || { std::thread::sleep(Duration::from_millis(200)); drop(held); });
        let found = fake.locate(Duration::from_secs(3));
        release.join().unwrap();
        assert_eq!(found.state, Where::Title);
    }
}
