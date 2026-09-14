//! The request-file protocol the injector's hooks share.
//!
//! A hook polls for `<stem>.req`, reads every line, removes the file and
//! answers in `<stem>.log`. Only MemProbe echoes a label; the others are
//! confirmed by what their log gained after the request was written.

use std::path::Path;
use std::time::{Duration, Instant};

/// Writes `body` to `<stem>.req` and waits until `parse` finds its answer in
/// what `<stem>.log` gained afterwards. `parse` returns `Ok(None)` while the
/// answer is incomplete and `Err` for an answer that is a refusal.
pub fn exchange<T>(dir: &Path, stem: &str, body: &str, timeout: Duration,
    mut parse: impl FnMut(&str) -> Result<Option<T>, String>) -> Result<T, String> {
    let deadline = Instant::now() + timeout;
    let request = dir.join(format!("{stem}.req"));
    let log = dir.join(format!("{stem}.log"));
    // The hooks know nothing of the lock: it keeps the harness's own writers
    // from overwriting each other's requests before a hook reads them.
    let _lock = loop {
        match crate::control::Lock::acquire(&dir.join(format!("{stem}.lock"))) {
            Ok(lock) => break lock,
            Err(e) if !e.starts_with("busy:") => return Err(format!("request_write_failed: {e}")),
            Err(_) if Instant::now() >= deadline => return Err(format!("probe_busy: {stem}.lock ocupado")),
            Err(_) => crate::control::sleep(Duration::from_millis(50))?,
        }
    };
    // A hook reads the file and then removes it; replacing it in between
    // would lose the new request unread. So a request waits for the last one to go.
    while request.exists() {
        if Instant::now() >= deadline { return Err(format!("request_not_consumed: um {stem}.req anterior não foi lido")); }
        crate::control::sleep(Duration::from_millis(100))?;
    }
    let from = std::fs::metadata(&log).map(|m| m.len()).unwrap_or(0);
    let temporary = dir.join(format!("{stem}.req.tmp"));
    std::fs::write(&temporary, body).and_then(|_| std::fs::rename(&temporary, &request))
        .map_err(|e| format!("request_write_failed: {e}"))?;
    loop {
        crate::control::check()?;
        if let Some(answer) = parse(&crate::probe::read_from(&log, from).unwrap_or_default())? { return Ok(answer); }
        if Instant::now() >= deadline {
            return Err(if request.exists() {
                format!("request_not_consumed: nada leu {stem}.req (jogo iniciando, ou DLL sem o hook)")
            } else { format!("no_answer: {stem}.req foi lido e a resposta não apareceu completa") });
        }
        crate::control::sleep(Duration::from_millis(100))?;
    }
}

/// A hook's log line without its `HH:MM:SS.mmm  ` clock and newline.
pub fn text(line: &str) -> &str {
    let line = line.trim_end_matches(['\n', '\r']);
    match line.split_once("  ") {
        Some((clock, rest)) if clock.len() == 12 && clock.as_bytes()[2] == b':' => rest,
        _ => line,
    }
}

/// The complete lines of an appended chunk, without clocks.
pub fn lines(appended: &str) -> Vec<&str> {
    appended.split_inclusive('\n').filter(|l| l.ends_with('\n')).map(text).collect()
}
