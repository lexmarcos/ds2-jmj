//! Reading logs that are not quite text.
//!
//! The DS3OS server draws its columns with bytes that are not valid UTF-8, so
//! grep calls the file binary and refuses to print matches unless told
//! otherwise. Everything here goes through a lossy read and normalises those
//! separators, so a log is always readable and always greppable.

use std::io::Read;
use std::path::Path;

/// Reads a log as text, whatever bytes are actually in it.
pub fn read_text(path: &Path) -> std::io::Result<String> {
    let mut bytes = Vec::new();
    std::fs::File::open(path)?.read_to_end(&mut bytes)?;
    Ok(sanitize(&String::from_utf8_lossy(&bytes)))
}

/// The whole lines a log gained past byte `from`, as text, and the byte to read
/// from next time. A line still being written is left for the next read. A log
/// shorter than `from` was recreated (the server does on every start) and is
/// read from the top.
pub fn read_lines_from(path: &Path, from: u64) -> std::io::Result<(String, u64)> {
    use std::io::{Seek, SeekFrom};
    let mut file = std::fs::File::open(path)?;
    let start = if file.metadata()?.len() < from { 0 } else { from };
    file.seek(SeekFrom::Start(start))?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)?;
    let complete = bytes.iter().rposition(|b| *b == b'\n').map(|k| k + 1).unwrap_or(0);
    Ok((sanitize(&String::from_utf8_lossy(&bytes[..complete])), start + complete as u64))
}

/// Turns the server's column separators into pipes and squeezes the padding,
/// which makes lines short enough to read and stable enough to match on.
fn sanitize(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for line in text.lines() {
        let replaced = line.replace('\u{fffd}', "|");
        let mut spaces = 0;
        for character in replaced.chars() {
            if character == ' ' {
                spaces += 1;
                if spaces > 2 {
                    continue;
                }
            } else {
                spaces = 0;
            }
            out.push(character);
        }
        out.push('\n');
    }
    out
}

pub struct Options<'a> {
    pub lines: usize,
    pub grep: Option<&'a str>,
    pub follow: bool,
}

/// Prints the tail of a log, optionally filtered, optionally following.
pub fn show(path: &Path, options: &Options<'_>) -> std::io::Result<()> {
    if !path.is_file() {
        println!("  {} ainda não existe", path.display());
        return Ok(());
    }

    let matches = |line: &str| match options.grep {
        Some(needle) => line.to_lowercase().contains(&needle.to_lowercase()),
        None => true,
    };

    let text = read_text(path)?;
    let selected: Vec<&str> = text.lines().filter(|line| matches(line)).collect();
    for line in selected.iter().rev().take(options.lines).rev() {
        println!("{line}");
    }

    if !options.follow {
        return Ok(());
    }

    let mut seen = text.len();
    loop {
        crate::control::sleep(std::time::Duration::from_millis(500)).map_err(std::io::Error::other)?;
        let text = read_text(path)?;
        if text.len() <= seen {
            // The file was rotated or truncated; start over from the top.
            if text.len() < seen {
                seen = 0;
            }
            continue;
        }
        for line in text[seen..].lines().filter(|line| matches(line)) {
            println!("{line}");
        }
        seen = text.len();
    }
}
