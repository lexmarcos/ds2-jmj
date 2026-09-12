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

const REQUEST: &str = "DS2_MemProbe.req";
const ANSWER: &str = "DS2_MemProbe.log";

/// Module relative address of the title state's "I am alive" byte.
const TITLE_FLAG: &str = "1614804";

/// Where the game is, as far as the injector can say.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Where {
    Title,
    World,
    /// No answer: the game may still be booting, or the injector is not in it.
    Unknown,
}

/// Reads the title flag out of one instance.
///
/// The injector polls for the request file twice a second, so an answer takes
/// about that long. Anything slower than `timeout` counts as unknown rather
/// than as an error: this is asked in a loop, and a driver that stops because
/// one read was slow is worse than one that tries again.
pub fn locate(install_dir: &Path, timeout: Duration) -> Where {
    let answer = install_dir.join(ANSWER);
    // Start from an empty answer, so an old reply is never read as this one.
    let _ = std::fs::write(&answer, b"");

    if write_request(install_dir, &format!("mod titleflag {TITLE_FLAG} 1\n")).is_err() {
        return Where::Unknown;
    }

    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if let Some(byte) = read_byte(&answer, "titleflag") {
            return if byte == 0 { Where::World } else { Where::Title };
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    Where::Unknown
}

/// Writes the request under a temporary name and moves it into place, so the
/// injector never reads half a file.
fn write_request(install_dir: &Path, body: &str) -> std::io::Result<()> {
    let temporary: PathBuf = install_dir.join("DS2_MemProbe.req.tmp");
    std::fs::write(&temporary, body)?;
    std::fs::rename(&temporary, install_dir.join(REQUEST))
}

/// The first byte reported for `label`, from a reply that looks like
///
/// ```text
///   titleflag: 0x0000000141614804 (1 bytes)
///     0000000141614804 +0x000 01
/// ```
fn read_byte(answer: &Path, label: &str) -> Option<u8> {
    let text = std::fs::read(answer).ok()?;
    let text = String::from_utf8_lossy(&text);
    let mut lines = text.lines();
    while let Some(line) = lines.next() {
        if !line.trim_start().starts_with(&format!("{label}:")) {
            continue;
        }
        let values = lines.next()?;
        let last = values.split_whitespace().last()?;
        return u8::from_str_radix(last, 16).ok();
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_byte_out_of_a_reply() {
        let dir = std::env::temp_dir().join(format!("ds2os-probe-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let answer = dir.join("reply.log");
        std::fs::write(
            &answer,
            "=== pedido titleflag ===\n  titleflag: 0x0000000141614804 (1 bytes)\n    \
             0000000141614804 +0x000 01\n",
        )
        .unwrap();
        assert_eq!(read_byte(&answer, "titleflag"), Some(1));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn an_answer_for_another_label_is_not_ours() {
        let dir = std::env::temp_dir().join(format!("ds2os-probe-other-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let answer = dir.join("reply.log");
        std::fs::write(
            &answer,
            "  areaid: 0x0000000141614800 (4 bytes)\n    0000000141614800 +0x000 00 00 00 00\n",
        )
        .unwrap();
        assert_eq!(read_byte(&answer, "titleflag"), None);
        std::fs::remove_dir_all(&dir).ok();
    }
}
