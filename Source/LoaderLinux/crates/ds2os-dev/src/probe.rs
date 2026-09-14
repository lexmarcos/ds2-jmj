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
    let title = Command::parse(&format!("mod titleflag {TITLE_FLAG} 1")).expect("a valid line");
    let exchange = exchange(install, expected, std::slice::from_ref(&title), deadline);
    found.request_written = exchange.request_written;
    found.label = exchange.labels.first().cloned();
    found.detail = exchange.detail;
    let answers = match exchange.outcome { Ok(answers) => answers, Err(reason) => return (UNKNOWN, reason) };
    let byte = match &answers[0].reply {
        Reply::Bytes { bytes, .. } if !bytes.is_empty() => bytes[0],
        Reply::Unreadable { .. } | Reply::ChainUnresolved => return (UNKNOWN, Unreadable),
        _ => return (UNKNOWN, MalformedAnswer),
    };
    found.byte = Some(byte);
    let dir = install.game_dir.as_path();
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

/// One MemProbe command, named by the caller. The harness turns the name into
/// a unique label, so neither an older reply nor another reader's can answer it.
///
/// ```text
/// abs       <name> <hex address> <length>
/// mod       <name> <hex module offset> <length>
/// chain     <name> <hex module offset> <hex offsets, comma separated> <length>
/// pokeabs   <name> <hex address> <hex bytes> [<hex expected bytes>]
/// pokemod   <name> <hex module offset> <hex bytes> [<hex expected bytes>]
/// pokechain <name> <hex module offset> <hex offsets> <hex bytes> [<hex expected bytes>]
/// scan      <name> <hex value> <width 1|2|4|8> [<max hits>]
/// ```
///
/// Two things in the injector's parser are easy to get wrong, and both fail
/// quietly: the length is **decimal** (`200` is 0xc8 bytes), and a chain
/// starts by dereferencing the module address itself, so its list holds only
/// the offsets after that — `chain chr 16148f0 d0 376` is `*(*(base+16148f0)+d0)`,
/// and a leading `0,` would dereference the context's vtable instead.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Command { pub kind: String, pub name: String, pub args: Vec<String> }

/// The injector falls back to 0x100 bytes outside this range, which would make
/// the reply longer or shorter than asked.
const MAX_LENGTH: usize = 0x4000;

impl Command {
    pub fn parse(raw: &str) -> Result<Self, String> {
        let bad = |why: &str| format!("invalid_probe_line: {raw:?}: {why}");
        let fields: Vec<&str> = raw.split_whitespace().collect();
        let (Some(kind), Some(name)) = (fields.first(), fields.get(1)) else { return Err(bad("tipo e nome são obrigatórios")); };
        if name.is_empty() || name.len() > 32 || !name.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
            return Err(bad("nome com letras, números e _, até 32"));
        }
        let args: Vec<String> = fields[2..].iter().map(|s| s.to_string()).collect();
        let hex = |s: &str| !s.is_empty() && s.len() <= 16 && s.bytes().all(|b| b.is_ascii_hexdigit());
        let bytes = |s: &str| !s.is_empty() && s.len() % 2 == 0 && s.len() <= 2 * MAX_LENGTH && s.bytes().all(|b| b.is_ascii_hexdigit());
        let offsets = |s: &str| s.split(',').all(hex);
        let length = |s: &str| s.parse::<usize>().is_ok_and(|n| (1..=MAX_LENGTH).contains(&n));
        let a = |i: usize| args.get(i).map(String::as_str).unwrap_or("");
        let ok = match (*kind, args.len()) {
            ("abs" | "mod", 2) => hex(a(0)) && length(a(1)),
            ("chain", 3) => hex(a(0)) && offsets(a(1)) && length(a(2)),
            ("pokeabs" | "pokemod", 2 | 3) => hex(a(0)) && bytes(a(1)) && (args.len() == 2 || a(2).len() == a(1).len() && bytes(a(2))),
            ("pokechain", 3 | 4) => hex(a(0)) && offsets(a(1)) && bytes(a(2)) && (args.len() == 3 || a(3).len() == a(2).len() && bytes(a(3))),
            ("scan", 2 | 3) => hex(a(0)) && matches!(a(1), "1" | "2" | "4" | "8") && (args.len() == 2 || a(2).parse::<u32>().is_ok_and(|n| (1..=4000).contains(&n))),
            ("abs" | "mod" | "chain" | "pokeabs" | "pokemod" | "pokechain" | "scan", _) => false,
            _ => return Err(bad("tipo desconhecido")),
        };
        if !ok { return Err(bad("argumentos inválidos (comprimento é decimal, de 1 a 16384; bytes em hex)")); }
        Ok(Self { kind: kind.to_string(), name: name.to_string(), args })
    }

    pub fn is_poke(&self) -> bool { self.kind.starts_with("poke") }

    /// The number of bytes a read asks for.
    fn length(&self) -> Option<usize> {
        match self.kind.as_str() { "abs" | "mod" | "chain" => self.args.last()?.parse().ok(), _ => None }
    }

    fn render(&self, label: &str) -> String { format!("{} {label} {}", self.kind, self.args.join(" ")) }
}

/// What the injector answered for one command.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Reply {
    Bytes { address: u64, bytes: Vec<u8> },
    /// `ilegivel` (with the address) or `nao resolvido` (address zero).
    Unreadable { address: Option<u64> },
    /// A chain hit a null or unreadable pointer on the way.
    ChainUnresolved,
    Poked { address: u64, before: Option<String>, wrote: bool },
    /// The expected bytes were not there, so nothing was written.
    PokeRefused { address: u64, expected: String, found: Option<String> },
    PokeInvalid,
    Scan { hits: Vec<u64> },
    /// A reply for the label in a shape this parser does not know.
    Malformed { line: String },
}

#[derive(Debug, Clone)]
pub struct Answer { pub name: String, pub label: String, pub reply: Reply }

impl Answer {
    pub fn json(&self) -> serde_json::Value {
        use serde_json::json;
        let hex = |b: &[u8]| b.iter().map(|x| format!("{x:02x}")).collect::<String>();
        let address = |a: &u64| format!("0x{a:016x}");
        let body = match &self.reply {
            Reply::Bytes { address: at, bytes } => json!({"kind": "bytes", "address": address(at), "bytes": hex(bytes)}),
            Reply::Unreadable { address: at } => json!({"kind": "unreadable", "address": at.as_ref().map(address)}),
            Reply::ChainUnresolved => json!({"kind": "chain_unresolved"}),
            Reply::Poked { address: at, before, wrote } => json!({"kind": "poked", "address": address(at), "before": before, "wrote": wrote}),
            Reply::PokeRefused { address: at, expected, found } => json!({"kind": "poke_refused", "address": address(at), "expected": expected, "found": found}),
            Reply::PokeInvalid => json!({"kind": "poke_invalid"}),
            Reply::Scan { hits } => json!({"kind": "scan", "hits": hits.iter().map(address).collect::<Vec<_>>()}),
            Reply::Malformed { line } => json!({"kind": "malformed", "line": line}),
        };
        let mut value = json!({"name": self.name, "label": self.label});
        value.as_object_mut().unwrap().extend(body.as_object().unwrap().clone());
        value
    }
}

/// One round trip to the injector.
pub struct Exchange {
    pub request_written: bool,
    pub labels: Vec<String>,
    /// Every command answered, in request order; or why not.
    pub outcome: Result<Vec<Answer>, Reason>,
    pub detail: Option<String>,
    pub elapsed_ms: u128,
}

/// Sends every command in one request and waits for all of their replies.
///
/// One request with many labels, never many requests in a row: the injector
/// looks for the file twice a second, so each round trip costs about half a
/// second however little it asks for.
pub fn request(install: &Install, commands: &[Command], timeout: Duration) -> Exchange {
    let started = Instant::now();
    let exchange = exchange(install, ds2os_core::exe::DS2_SOTFS_1_03, commands, started + timeout);
    crate::output::event("probe", serde_json::json!({
        "instance": install.account, "requestWritten": exchange.request_written, "elapsedMs": exchange.elapsed_ms,
        "lines": commands.iter().zip(exchange.labels.iter().map(Some).chain(std::iter::repeat(None)))
            .map(|(c, label)| c.render(label.map(String::as_str).unwrap_or(&c.name))).collect::<Vec<_>>(),
        "answers": exchange.outcome.as_ref().ok().map(|a| a.iter().map(Answer::json).collect::<Vec<_>>()),
        "reason": exchange.outcome.as_ref().err(), "detail": exchange.detail,
    }));
    exchange
}

fn exchange(install: &Install, expected: Fingerprint, commands: &[Command], deadline: Instant) -> Exchange {
    let started = Instant::now();
    let mut exchange = Exchange { request_written: false, labels: Vec::new(), outcome: Err(Reason::Timeout), detail: None, elapsed_ms: 0 };
    exchange.outcome = round_trip(install, expected, commands, deadline, &mut exchange);
    exchange.elapsed_ms = started.elapsed().as_millis();
    exchange
}

fn round_trip(install: &Install, expected: Fingerprint, commands: &[Command], deadline: Instant, exchange: &mut Exchange)
    -> Result<Vec<Answer>, Reason> {
    use Reason::*;
    match crate::observe::build_of(install, expected) {
        Build::Expected => {}
        Build::Missing => return Err(ExeMissing),
        Build::Other => return Err(UnsupportedExe),
    }
    let dir = install.game_dir.as_path();
    // `observe` and `doctor` run beside a controller, so a held lock is waited
    // for rather than taken as an answer.
    let _lock = loop {
        match crate::control::Lock::acquire(&dir.join(LOCK)) {
            Ok(lock) => break lock,
            Err(e) if !e.starts_with("busy:") => { exchange.detail = Some(e); return Err(RequestWriteFailed); }
            Err(_) if Instant::now() >= deadline => return Err(ProbeBusy),
            Err(_) => if crate::control::sleep(Duration::from_millis(50)).is_err() { return Err(Cancelled); },
        }
    };
    let answer = dir.join(ANSWER);
    // Only what is appended after the request can answer it, so the log, which
    // grows by megabytes over a day, is not read whole on every poll.
    let from = std::fs::metadata(&answer).map(|m| m.len()).unwrap_or(0);
    let id = crate::output::id();
    exchange.labels = commands.iter().map(|c| format!("{}-{id}", c.name)).collect();
    let body: String = commands.iter().zip(&exchange.labels).map(|(c, label)| c.render(label) + "\n").collect();
    if let Err(e) = write_request(dir, &body) {
        exchange.detail = Some(e.to_string());
        return Err(RequestWriteFailed);
    }
    exchange.request_written = true;

    loop {
        if crate::control::check().is_err() { return Err(Cancelled); }
        let appended = read_from(&answer, from).unwrap_or_default();
        let replies = parse_replies(&appended, commands, &exchange.labels);
        if replies.iter().all(Option::is_some) {
            return Ok(replies.into_iter().zip(commands).zip(&exchange.labels)
                .map(|((reply, c), label)| Answer { name: c.name.clone(), label: label.clone(), reply: reply.unwrap() }).collect());
        }
        if Instant::now() >= deadline {
            let answered = replies.iter().filter(|r| r.is_some()).count();
            if answered > 0 { exchange.detail = Some(format!("{answered} de {} respondidos", commands.len())); }
            return Err(if dir.join(REQUEST).exists() { RequestNotConsumed } else { NoAnswer });
        }
        if crate::control::sleep(Duration::from_millis(100)).is_err() { return Err(Cancelled); }
    }
}

/// Writes the request under a temporary name and moves it into place, so the
/// injector never reads half a file.
fn write_request(install_dir: &Path, body: &str) -> std::io::Result<()> {
    let temporary: PathBuf = install_dir.join("DS2_MemProbe.req.tmp");
    std::fs::write(&temporary, body)?;
    std::fs::rename(&temporary, install_dir.join(REQUEST))
}

/// What the log gained after byte `from`. A log that shrank was replaced, and
/// its whole content is new.
fn read_from(path: &Path, from: u64) -> Option<String> {
    use std::io::{Read, Seek, SeekFrom};
    let mut file = std::fs::File::open(path).ok()?;
    let start = if file.metadata().ok()?.len() < from { 0 } else { from };
    file.seek(SeekFrom::Start(start)).ok()?;
    let mut raw = Vec::new();
    file.read_to_end(&mut raw).ok()?;
    Some(String::from_utf8_lossy(&raw).into_owned())
}

/// The reply for each label, in the shapes `DS2_MemProbeHook` writes:
///
/// ```text
/// === pedido L ===
///   L: 0x0000000141614804 (20 bytes)
///     0000000141614804 +0x000 01000000 00000000 00000000 00000000
///     0000000141614814 +0x010 00000000
///   L: 0x0000000141614804 ilegivel
///   L: nao resolvido
/// === L: cadeia nao resolveu ===
/// === poke L 0x0000000141614804 00 antes=01 ok ===            (or FALHOU)
/// === poke L 0x0000000141614804 RECUSADO esperava=02 achou=01 ===
/// === poke L: bytes invalidos 'zz' ===
/// === varredura L: 2 ocorrencias em 812.0 MB ===
///     0x00007fffeb897d50
/// ```
///
/// A reply counts only when every line of it has arrived with its newline.
fn parse_replies(text: &str, commands: &[Command], labels: &[String]) -> Vec<Option<Reply>> {
    let lines: Vec<&str> = text.split_inclusive('\n').collect();
    let complete = |i: usize| lines.get(i).filter(|l| l.ends_with('\n')).map(|l| l.trim_end_matches(['\n', '\r']));
    let mut replies: Vec<Option<Reply>> = vec![None; labels.len()];
    let index = |label: &str| labels.iter().position(|l| l == label);
    let mut i = 0;
    while let Some(line) = complete(i) {
        i += 1;
        let Some(inner) = line.strip_prefix("=== ").and_then(|l| l.strip_suffix(" ===")) else { continue };
        if let Some(label) = inner.strip_prefix("pedido ") {
            let Some(at) = index(label) else { continue };
            let Some(head) = complete(i) else { break };
            let Some(rest) = head.trim_start().strip_prefix(&format!("{label}:")).map(str::trim) else {
                replies[at].get_or_insert(Reply::Malformed { line: head.to_owned() });
                continue;
            };
            i += 1;
            if rest == "nao resolvido" { replies[at].get_or_insert(Reply::Unreadable { address: None }); continue; }
            let address = rest.split_whitespace().next().and_then(parse_address);
            if rest.ends_with("ilegivel") { replies[at].get_or_insert(Reply::Unreadable { address }); continue; }
            let count = rest.split_whitespace().nth(1).and_then(|n| n.strip_prefix('(')).and_then(|n| n.parse::<usize>().ok());
            let (Some(address), Some(count)) = (address, count) else {
                replies[at].get_or_insert(Reply::Malformed { line: head.to_owned() });
                continue;
            };
            if commands[at].length().is_some_and(|asked| asked != count) {
                replies[at].get_or_insert(Reply::Malformed { line: head.to_owned() });
                continue;
            }
            let (mut bytes, mut arrived, mut readable) = (Vec::with_capacity(count), true, true);
            for _ in 0..count.div_ceil(16) {
                let Some(dump) = complete(i) else { arrived = false; break };
                i += 1;
                // The dump puts a space after every fourth byte.
                let digits: String = dump.split_whitespace().skip(2).collect();
                match decode_hex(&digits) { Some(row) => bytes.extend(row), None => { readable = false; break } }
            }
            // The rest of this reply is still being written; nothing after it can be complete.
            if !arrived { break; }
            replies[at].get_or_insert(if readable && bytes.len() == count { Reply::Bytes { address, bytes } }
                else { Reply::Malformed { line: head.to_owned() } });
        } else if let Some(label) = inner.strip_suffix(": cadeia nao resolveu") {
            if let Some(at) = index(label) { replies[at].get_or_insert(Reply::ChainUnresolved); }
        } else if let Some(poke) = inner.strip_prefix("poke ") {
            let fields: Vec<&str> = poke.split_whitespace().collect();
            if let Some(label) = fields.first().and_then(|f| f.strip_suffix(':')) {
                if let Some(at) = index(label) { replies[at].get_or_insert(Reply::PokeInvalid); }
                continue;
            }
            let Some(at) = fields.first().and_then(|label| index(label)) else { continue };
            let address = fields.get(1).and_then(|a| parse_address(a));
            let field = |key: &str| fields.iter().find_map(|f| f.strip_prefix(key)).map(str::to_owned);
            let reply = match (address, fields.get(2).copied(), fields.last().copied()) {
                (Some(address), Some("RECUSADO"), _) => Reply::PokeRefused { address,
                    expected: field("esperava=").unwrap_or_default(), found: field("achou=").filter(|f| f != "ilegivel") },
                (Some(address), _, Some(result @ ("ok" | "FALHOU"))) => Reply::Poked { address,
                    before: field("antes=").filter(|b| b != "?"), wrote: result == "ok" },
                _ => Reply::Malformed { line: line.to_owned() },
            };
            replies[at].get_or_insert(reply);
        } else if let Some(scan) = inner.strip_prefix("varredura ") {
            let Some((label, rest)) = scan.split_once(": ") else { continue };
            let Some(at) = index(label) else { continue };
            let Some(count) = rest.split_whitespace().next().and_then(|n| n.parse::<usize>().ok()) else {
                replies[at].get_or_insert(Reply::Malformed { line: line.to_owned() });
                continue;
            };
            let hits: Option<Vec<u64>> = (0..count).map(|k| complete(i + k).and_then(|l| parse_address(l.trim()))).collect();
            let Some(hits) = hits else { break };
            i += count;
            replies[at].get_or_insert(Reply::Scan { hits });
        }
    }
    replies
}

fn decode_hex(digits: &str) -> Option<Vec<u8>> {
    if digits.len() % 2 != 0 || !digits.is_ascii() { return None; }
    (0..digits.len() / 2).map(|k| u8::from_str_radix(&digits[2 * k..2 * k + 2], 16).ok()).collect()
}

fn parse_address(text: &str) -> Option<u64> {
    u64::from_str_radix(text.strip_prefix("0x")?, 16).ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{atomic::{AtomicBool, Ordering}, Arc};

    fn commands(lines: &[&str]) -> Vec<Command> { lines.iter().map(|l| Command::parse(l).unwrap()).collect() }

    #[test]
    fn lines_the_injector_would_misread_are_refused_before_writing() {
        assert!(Command::parse("chain chr 16148f0 d0 376").is_ok());
        assert!(Command::parse("pokeabs hp 7fffeb7b9fc8 00000000 64030000").is_ok());
        assert!(Command::parse("scan s 1410d7998 8 6").is_ok());
        for bad in ["abs r 141614804 0", "abs r 141614804 99999", "abs r 141614804 0x10", "chain c 16148f0 d0",
            "pokeabs p 1000 0", "pokeabs p 1000 00 0000", "scan s 10 3", "peek r 10 1", "abs r-1 10 1", "abs"] {
            assert!(Command::parse(bad).is_err(), "{bad}");
        }
        assert!(Command::parse("pokemod p 250e5b b001c3").unwrap().is_poke());
    }

    #[test]
    fn every_reply_shape_the_injector_writes_is_read() {
        let commands = commands(&["mod title 1614804 1", "chain chr 16148f0 d0 20", "abs gone 0 4", "abs bad 10 4",
            "chain far 16148f0 d0,490 4", "pokeabs hp 7fffeb7b9fc8 00000000 64030000", "pokeabs hp2 7fffeb7b9fc8 00000000 01000000",
            "scan s 1410d7998 8 6", "pokeabs inv 10 00"]);
        let labels: Vec<String> = commands.iter().map(|c| format!("{}-7", c.name)).collect();
        let log = "\n=== pedido title-7 ===\n  title-7: 0x0000000141614804 (1 bytes)\n    0000000141614804 +0x000 01\n\
            \n=== pedido chr-7 ===\n  chr-7: 0x00007fffeb7b9e60 (20 bytes)\n    00007fffeb7b9e60 +0x000 01020304 05060708 090a0b0c 0d0e0f10\n    00007fffeb7b9e70 +0x010 11121314\n\
            \n=== pedido gone-7 ===\n  gone-7: nao resolvido\n\
            \n=== pedido bad-7 ===\n  bad-7: 0x0000000000000010 ilegivel\n\
            \n=== far-7: cadeia nao resolveu ===\n\
            \n=== poke hp-7 0x00007fffeb7b9fc8 00000000 antes=64030000 ok ===\n\
            \n=== poke hp2-7 0x00007fffeb7b9fc8 RECUSADO esperava=01000000 achou=64030000 ===\n\
            \n=== varredura s-7: 2 ocorrencias em 812.0 MB ===\n    0x00007fffeb897d50\n    0x0000000141000000\n\
            \n=== poke inv-7: bytes invalidos 'zz' ===\n";
        let replies: Vec<Reply> = parse_replies(log, &commands, &labels).into_iter().map(Option::unwrap).collect();
        assert_eq!(replies, vec![
            Reply::Bytes { address: 0x141614804, bytes: vec![1] },
            Reply::Bytes { address: 0x7fffeb7b9e60, bytes: (1..=20).collect() },
            Reply::Unreadable { address: None },
            Reply::Unreadable { address: Some(0x10) },
            Reply::ChainUnresolved,
            Reply::Poked { address: 0x7fffeb7b9fc8, before: Some("64030000".into()), wrote: true },
            Reply::PokeRefused { address: 0x7fffeb7b9fc8, expected: "01000000".into(), found: Some("64030000".into()) },
            Reply::Scan { hits: vec![0x7fffeb897d50, 0x141000000] },
            Reply::PokeInvalid,
        ]);
    }

    #[test]
    fn a_reply_counts_only_when_it_is_ours_and_complete() {
        let commands = commands(&["chain chr 16148f0 d0 20"]);
        let labels = vec!["chr-new".to_owned()];
        let whole = "\n=== pedido chr-new ===\n  chr-new: 0x00007fffeb7b9e60 (20 bytes)\n    00007fffeb7b9e60 +0x000 01020304 05060708 090a0b0c 0d0e0f10\n    00007fffeb7b9e70 +0x010 11121314\n";
        // Another request's label, even one that shares the name.
        assert_eq!(parse_replies(&whole.replace("chr-new", "chr-old"), &commands, &labels), vec![None]);
        // Cut anywhere before the last newline, it is still arriving.
        for cut in [whole.len() - 1, whole.find("+0x010").unwrap(), whole.find("(20").unwrap()] {
            assert_eq!(parse_replies(&whole[..cut], &commands, &labels), vec![None], "{cut}");
        }
        assert!(matches!(parse_replies(whole, &commands, &labels)[0], Some(Reply::Bytes { .. })));
        // A length other than the one asked for is not silently accepted.
        assert!(matches!(parse_replies(&whole.replace("(20 bytes)", "(256 bytes)"), &commands, &labels)[0], Some(Reply::Malformed { .. })));
    }

    #[test]
    fn only_what_follows_the_request_is_read() {
        let path = std::env::temp_dir().join(format!("ds2-reply-{}", crate::output::id()));
        std::fs::write(&path, "old reply\n").unwrap();
        std::fs::OpenOptions::new().append(true).open(&path).and_then(|mut f| { use std::io::Write; f.write_all(b"new\n") }).unwrap();
        assert_eq!(read_from(&path, 10).as_deref(), Some("new\n"));
        // A log replaced by a shorter one is read from its start.
        std::fs::write(&path, "short\n").unwrap();
        assert_eq!(read_from(&path, 1 << 20).as_deref(), Some("short\n"));
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
        /// removes it, and appends whatever `reply` writes for each line.
        fn injector(&self, reply: impl Fn(&str) -> String + Send + 'static) {
            let (root, stop) = (self.root.clone(), self.stop.clone());
            std::thread::spawn(move || while !stop.load(Ordering::Relaxed) {
                let request = root.join(REQUEST);
                if let Ok(body) = std::fs::read_to_string(&request) {
                    std::fs::remove_file(&request).unwrap();
                    use std::io::Write;
                    let mut log = std::fs::OpenOptions::new().create(true).append(true).open(root.join(ANSWER)).unwrap();
                    for line in body.lines() {
                        write!(log, "{}", reply(line)).unwrap();
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

    fn label(line: &str) -> String { line.split_whitespace().nth(1).unwrap().to_owned() }

    fn dump(line: &str, byte: u8) -> String {
        let label = label(line);
        format!("\n=== pedido {label} ===\n  {label}: 0x0000000141614804 (1 bytes)\n    0000000141614804 +0x000 {byte:02x}\n")
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

        fake.injector(|line| format!("\n=== pedido {} ===\n", label(line)));
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
    fn one_request_carries_every_line_and_a_refused_write_is_not_a_write() {
        let fake = FakeInstall::new();
        fake.injector(|line| if line.starts_with("pokeabs") {
            format!("\n=== poke {} 0x0000000000001000 RECUSADO esperava=01 achou=02 ===\n", label(line))
        } else { dump(line, 7) });
        let lines = commands(&["abs a 1000 1", "pokeabs p 1000 00 01"]);
        let exchange = exchange(&fake.install, fake.build, &lines, Instant::now() + Duration::from_secs(3));
        assert!(exchange.request_written);
        let answers = exchange.outcome.unwrap();
        assert_eq!((answers[0].name.as_str(), &answers[0].reply), ("a", &Reply::Bytes { address: 0x141614804, bytes: vec![7] }));
        assert_eq!(answers[1].name, "p");
        assert!(matches!(answers[1].reply, Reply::PokeRefused { .. }));
        assert_ne!(answers[0].label, answers[1].label);
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
