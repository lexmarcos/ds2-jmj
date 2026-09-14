//! Whether the two instances share a multiplayer session, and whether their
//! peers are actually exchanging data.
//!
//! Two levels, and the contract says which is which:
//!
//! 1. **The objects agree.** The host's `NetSummonAcceptMultiplayCtrl`
//!    (vftable `0x1410d7998`) has state `0x10` at `+0x150`, the guest's
//!    `NetSummonJoinMultiplayCtrl` (`0x1410d7bd8`) has state 7 at `+0xf8`, and
//!    `DS2_CoopChannelHook` on both machines lists the same two members with
//!    the same one marked host. This says a session exists. It does not say it
//!    works: the host's controller was measured still at `0x10` minutes after
//!    the session was gone.
//! 2. **The peers talk.** Between two channel samples at least 2.5 s apart the
//!    host's `enviados` grows with `falhas` unchanged and the guest's
//!    `recebidos` grows. The host announces its bonfire to the session every two
//!    seconds over Steam P2P, so this is data crossing from one peer to the other.
//!
//! `p2pSessionVerified` is `true` only with both levels on both instances in
//! the same sample, `false` only when neither instance shows a session or an
//! active controller, and `null` otherwise, with the reason in `problems`.

use std::time::{Duration, Instant};

use serde::Serialize;
use serde_json::json;

use crate::env::{Environment, Install};
use crate::hook_request;
use crate::probe::{self, Command, Reply};

const CHANNEL: &str = "DS2_Channel";
const HOST_VFTABLE: u64 = 0x1410d7998;
const GUEST_VFTABLE: u64 = 0x1410d7bd8;
const HOST_STATE: u64 = 0x150;
const GUEST_STATE: u64 = 0xf8;
const HOST_ACTIVE: i32 = 0x10;
const GUEST_ACTIVE: i32 = 7;
/// The channel keeps a session's members this long after it last saw them.
const MEMBERS_FRESH_MS: u64 = 5000;
/// The game publishes the local player every frame; older means a load.
const LOCAL_STALE_MS: u64 = 2000;
const SAMPLE_GAP: Duration = Duration::from_millis(2500);

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Member { pub steam_id: String, pub host: bool }

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ChannelSession { pub address: String, pub seen_ms: u64, pub members: Vec<Member> }

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Local { pub age_ms: u64, pub role: u32, pub map: String, pub id: String }

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Channel {
    pub polls: u64,
    pub foreign: u64,
    pub sent: u64,
    pub failed: u64,
    pub received: u64,
    pub refused: u64,
    /// This machine's account, as SteamID64 decimal. The hook only learns it
    /// while polling a session: outside one it reports zero, which is `null` here.
    pub me: Option<String>,
    pub sessions: Vec<ChannelSession>,
    pub local: Option<Local>,
}

fn decimal(hex: &str) -> Option<String> { u64::from_str_radix(hex, 16).ok().map(|id| id.to_string()) }

/// The first complete `status` block in what `DS2_Channel.log` gained:
///
/// ```text
/// 13:30:30.100  === canal 7: polls=4657 estranhos=0 enviados=22 falhas=0 recebidos=0 recusados=0 eu=011000010afd1a3a ===
///     sessao 00007FFFFE5BFA00 vista ha 3 ms: 2 membros: 011000010afd1a3a (host) 0110000140d6d6d1
///     local ha 7 ms: papel 0, registro mapa 0a1f0000 tipo 0 id 00007ba7
///     do host: nada recebido
/// ```
///
/// The hook writes the block in one append and always ends it with the
/// `do host` line, so a block without it is still arriving.
pub fn parse_channel(appended: &str) -> Result<Option<Channel>, String> {
    let lines: Vec<&str> = hook_request::lines(appended);
    let Some(start) = lines.iter().position(|l| l.starts_with("=== canal ")) else { return Ok(None) };
    let Some(end) = lines[start + 1..].iter().position(|l| l.trim_start().starts_with("do host")).map(|k| start + 1 + k) else { return Ok(None) };
    let malformed = |line: &str| format!("malformed_answer: {line:?}");
    let header = lines[start];
    let fields: std::collections::HashMap<&str, &str> = header.trim_end_matches(" ===").split_whitespace().filter_map(|f| f.split_once('=')).collect();
    let count = |key: &str| fields.get(key).and_then(|v| v.parse::<u64>().ok()).ok_or_else(|| malformed(header));
    let mut channel = Channel {
        polls: count("polls")?, foreign: count("estranhos")?, sent: count("enviados")?, failed: count("falhas")?,
        received: count("recebidos")?, refused: count("recusados")?,
        me: match fields.get("eu").and_then(|v| decimal(v)).ok_or_else(|| malformed(header))? {
            zero if zero == "0" => None,
            id => Some(id),
        },
        sessions: Vec::new(), local: None,
    };
    for line in &lines[start + 1..end] {
        let line = line.trim_start();
        if let Some(rest) = line.strip_prefix("sessao ") {
            // sessao 00007FFFFE5BFA00 vista ha 3 ms: 2 membros: 011000010afd1a3a (host) 0110000140d6d6d1
            let (head, members) = rest.split_once(" membros:").ok_or_else(|| malformed(line))?;
            let words: Vec<&str> = head.split_whitespace().collect();
            let (Some(address), Some(seen)) = (words.first(), words.get(3).and_then(|v| v.parse::<u64>().ok())) else { return Err(malformed(line)) };
            let tokens: Vec<&str> = members.split_whitespace().filter(|t| *t != "nenhum").collect();
            let mut list = Vec::new();
            for token in tokens {
                if token == "(host)" {
                    list.last_mut().map(|m: &mut Member| m.host = true).ok_or_else(|| malformed(line))?;
                } else {
                    list.push(Member { steam_id: decimal(token).ok_or_else(|| malformed(line))?, host: false });
                }
            }
            channel.sessions.push(ChannelSession { address: format!("0x{}", address.to_ascii_lowercase()), seen_ms: seen, members: list });
        } else if let Some(rest) = line.strip_prefix("local ha ") {
            // local ha 7 ms: papel 0, registro mapa 0a1f0000 tipo 0 id 00007ba7
            let words: Vec<&str> = rest.split_whitespace().collect();
            let word = |after: &str| words.iter().position(|w| *w == after).and_then(|k| words.get(k + 1)).map(|w| w.trim_end_matches(','));
            channel.local = Some(Local {
                age_ms: words.first().and_then(|v| v.parse().ok()).ok_or_else(|| malformed(line))?,
                role: word("papel").and_then(|v| v.parse().ok()).ok_or_else(|| malformed(line))?,
                map: word("mapa").ok_or_else(|| malformed(line))?.to_owned(),
                id: word("id").ok_or_else(|| malformed(line))?.to_owned(),
            });
        }
    }
    Ok(Some(channel))
}

pub fn channel(install: &Install, timeout: Duration) -> Result<Channel, String> {
    hook_request::exchange(&install.game_dir, CHANNEL, "status\n", timeout, parse_channel)
}

#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Controller { pub address: String, pub state: i32 }

#[derive(Debug, Clone, Default, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Controllers { pub host: Vec<Controller>, pub guest: Vec<Controller> }

/// Heap objects only: the vftable itself lives in the module, and code in the
/// module refers to it.
fn object(address: u64) -> bool { address >= 0x10000 && !(0x140000000..0x150000000).contains(&address) }

/// Two round trips: one with both scans, one reading every candidate's vftable
/// again and its state. A candidate whose first eight bytes no longer hold the
/// vftable was a copy of the needle, not an object.
pub fn controllers(install: &Install, timeout: Duration) -> Result<Controllers, String> {
    let deadline = Instant::now() + timeout;
    let scans = [format!("scan host {HOST_VFTABLE:x} 8 6"), format!("scan guest {GUEST_VFTABLE:x} 8 6")]
        .iter().map(|l| Command::parse(l)).collect::<Result<Vec<_>, _>>()?;
    let left = || deadline.saturating_duration_since(Instant::now());
    let exchange = probe::request(install, &scans, left());
    let answers = exchange.outcome.map_err(|r| format!("{r}: varredura das máquinas de sessão"))?;
    let hits = |at: usize| match &answers[at].reply { Reply::Scan { hits } => Ok(hits.iter().copied().filter(|a| object(*a)).collect::<Vec<u64>>()),
        other => Err(format!("malformed_answer: varredura {other:?}")) };
    let (mut host, mut guest) = (hits(0)?, hits(1)?);
    // The scan's own copy of the needle turns up in both lists at the same
    // address (measured 14/09, a low address in the injector), and after the
    // second scan it even holds the guest's vftable. No object has two.
    let both: Vec<u64> = host.iter().copied().filter(|a| guest.contains(a)).collect();
    host.retain(|a| !both.contains(a));
    guest.retain(|a| !both.contains(a));
    let mut reads = Vec::new();
    for (kind, list, offset) in [("h", &host, HOST_STATE), ("g", &guest, GUEST_STATE)] {
        for (k, address) in list.iter().enumerate() {
            reads.push(Command::parse(&format!("abs {kind}v{k} {address:x} 8"))?);
            reads.push(Command::parse(&format!("abs {kind}s{k} {:x} 4", address + offset))?);
        }
    }
    let mut found = Controllers::default();
    if reads.is_empty() { return Ok(found); }
    let exchange = probe::request(install, &reads, left());
    let answers = exchange.outcome.map_err(|r| format!("{r}: estado das máquinas de sessão"))?;
    for pair in answers.chunks(2) {
        let (Reply::Bytes { address, bytes: vftable }, Reply::Bytes { bytes: state, .. }) = (&pair[0].reply, &pair[1].reply) else { continue };
        let is_host = pair[0].name.starts_with('h');
        let expected = if is_host { HOST_VFTABLE } else { GUEST_VFTABLE };
        if u64::from_le_bytes(vftable[..8].try_into().unwrap()) != expected { continue; }
        let controller = Controller { address: format!("0x{address:016x}"), state: i32::from_le_bytes(state[..4].try_into().unwrap()) };
        if is_host { found.host.push(controller) } else { found.guest.push(controller) }
    }
    Ok(found)
}

/// Everything read from one instance.
#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Sample {
    pub instance: u8,
    pub configured_id: Option<String>,
    pub first: Channel,
    pub second: Channel,
    pub elapsed_ms: u128,
    pub controllers: Controllers,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Role { Host, Guest, None, Unknown }

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct View {
    pub instance: u8,
    pub role: Role,
    pub members: Vec<Member>,
    pub host_state: Option<i32>,
    pub guest_state: Option<i32>,
    pub sent: u64,
    pub failed: u64,
    pub received: u64,
    pub refused: u64,
    pub local_age_ms: Option<u64>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Assessment {
    pub instances: Vec<View>,
    /// The objects and both channels agree on one session between the two accounts.
    pub objects_agree: bool,
    /// Data crossed from the host to the guest between the samples.
    pub peers_exchange: bool,
    pub p2p_session_verified: Option<bool>,
    pub problems: Vec<String>,
}

fn fresh_session(channel: &Channel) -> Option<&ChannelSession> {
    channel.sessions.iter().filter(|s| s.seen_ms <= MEMBERS_FRESH_MS && s.members.len() >= 2).min_by_key(|s| s.seen_ms)
}

fn view(sample: &Sample) -> View {
    let session = fresh_session(&sample.second);
    let active_host = sample.controllers.host.iter().any(|c| c.state == HOST_ACTIVE);
    let active_guest = sample.controllers.guest.iter().any(|c| c.state == GUEST_ACTIVE);
    let role = match session {
        Some(s) => match s.members.iter().filter(|m| m.host).collect::<Vec<_>>().as_slice() {
            [host] if Some(&host.steam_id) == sample.second.me.as_ref() => Role::Host,
            [_] => Role::Guest,
            _ => Role::Unknown,
        },
        None if !active_host && !active_guest => Role::None,
        None => Role::Unknown,
    };
    let delta = |f: fn(&Channel) -> u64| f(&sample.second).saturating_sub(f(&sample.first));
    View {
        instance: sample.instance, role,
        members: session.map(|s| s.members.clone()).unwrap_or_default(),
        host_state: sample.controllers.host.iter().map(|c| c.state).find(|s| *s == HOST_ACTIVE).or(sample.controllers.host.first().map(|c| c.state)),
        guest_state: sample.controllers.guest.iter().map(|c| c.state).find(|s| *s == GUEST_ACTIVE).or(sample.controllers.guest.first().map(|c| c.state)),
        sent: delta(|c| c.sent), failed: delta(|c| c.failed), received: delta(|c| c.received), refused: delta(|c| c.refused),
        local_age_ms: sample.second.local.as_ref().map(|l| l.age_ms),
    }
}

/// The decision, from samples of both instances.
pub fn assess(samples: &[Sample]) -> Assessment {
    let views: Vec<View> = samples.iter().map(view).collect();
    let mut problems = Vec::new();
    for sample in samples {
        if let (Some(id), Some(me)) = (&sample.configured_id, &sample.second.me) {
            if id != me {
                problems.push(format!("identity_mismatch: instância {} roda a conta {me} e está configurada como {id}", sample.instance));
            }
        }
        if sample.elapsed_ms < SAMPLE_GAP.as_millis() {
            problems.push(format!("samples_too_close: instância {}, {} ms entre as amostras do canal", sample.instance, sample.elapsed_ms));
        }
    }
    let nobody = views.iter().all(|v| v.role == Role::None);
    let host = views.iter().find(|v| v.role == Role::Host);
    let guest = views.iter().find(|v| v.role == Role::Guest);
    let mut objects_agree = false;
    let mut peers_exchange = false;
    if samples.len() != 2 {
        problems.push("instances_missing: a verificação exige as duas instâncias".into());
    } else if let (Some(host), Some(guest)) = (host, guest) {
        let ids = |v: &View| { let mut ids: Vec<(String, bool)> = v.members.iter().map(|m| (m.steam_id.clone(), m.host)).collect(); ids.sort(); ids };
        let accounts: Vec<&str> = samples.iter().filter_map(|s| s.second.me.as_deref()).collect();
        let same_members = ids(host) == ids(guest);
        let only_us = host.members.len() == 2 && host.members.iter().all(|m| accounts.contains(&m.steam_id.as_str()));
        let states = host.host_state == Some(HOST_ACTIVE) && guest.guest_state == Some(GUEST_ACTIVE);
        if !same_members { problems.push("members_disagree: os dois canais listam membros ou host diferentes".into()); }
        if !only_us { problems.push(format!("not_our_pair: a sessão tem {} membros, e só duas contas são testáveis nesta máquina", host.members.len())); }
        if host.host_state != Some(HOST_ACTIVE) { problems.push(format!("host_controller_inactive: estado {:?}, esperado 0x10", host.host_state)); }
        if guest.guest_state != Some(GUEST_ACTIVE) { problems.push(format!("guest_controller_inactive: estado {:?}, esperado 7", guest.guest_state)); }
        objects_agree = same_members && only_us && states && problems.iter().all(|p| !p.starts_with("identity_mismatch"));
        peers_exchange = host.sent > 0 && host.failed == 0 && guest.received > 0;
        if !peers_exchange {
            problems.push(format!("channel_counters_stalled: host enviou {} (falhas {}), convidado recebeu {}", host.sent, host.failed, guest.received));
            if host.local_age_ms.is_some_and(|age| age > LOCAL_STALE_MS) {
                problems.push("host_loading: o host não publica o quadro local há mais de 2 s e para de anunciar".into());
            }
        }
        if objects_agree && !peers_exchange { problems.push("session_objects_only: a sessão existe, mas nenhum dado atravessou entre as amostras".into()); }
    } else if !nobody {
        problems.push(format!("roles_incomplete: papéis {:?}", views.iter().map(|v| v.role).collect::<Vec<_>>()));
    }
    let verified = if samples.len() == 2 && objects_agree && peers_exchange && problems.iter().all(|p| !p.starts_with("samples_too_close")) {
        Some(true)
    } else if samples.len() == 2 && nobody && problems.is_empty() {
        Some(false)
    } else { None };
    Assessment { instances: views, objects_agree, peers_exchange, p2p_session_verified: verified, problems }
}

/// Samples both instances in parallel and decides.
pub fn observe(env: &Environment, timeout: Duration) -> Result<(Assessment, Vec<Sample>), String> {
    let settings = crate::settings::HarnessConfig::load();
    let installs: Vec<&Install> = [1u8, 2].iter().filter_map(|a| env.installs.iter().find(|i| i.account == *a)).collect();
    if installs.len() != 2 { return Err("instances_missing: a sessão exige as duas instalações".into()); }
    for install in &installs {
        if crate::observe::processes(env, install.account).is_empty() {
            return Err(format!("instance_stopped: conta {} sem processo do jogo", install.account));
        }
    }
    let deadline = Instant::now() + timeout;
    let left = move || deadline.saturating_duration_since(Instant::now());
    let results: Vec<Result<Sample, String>> = std::thread::scope(|scope| {
        let handles: Vec<_> = installs.iter().map(|install| {
            let configured_id = settings.steam_ids.get(&install.account).cloned();
            scope.spawn(move || -> Result<Sample, String> {
                let started = Instant::now();
                let first = channel(install, left())?;
                let controllers = controllers(install, left())?;
                let wait = SAMPLE_GAP.saturating_sub(started.elapsed());
                crate::control::sleep(wait)?;
                let second = channel(install, left())?;
                Ok(Sample { instance: install.account, configured_id, first, second, elapsed_ms: started.elapsed().as_millis(), controllers })
            })
        }).collect();
        handles.into_iter().map(|h| h.join().unwrap_or_else(|_| Err("panic ao amostrar a sessão".into()))).collect()
    });
    let mut samples = Vec::new();
    for (install, result) in installs.iter().zip(results) {
        samples.push(result.map_err(|e| format!("conta {}: {e}", install.account))?);
    }
    let assessment = assess(&samples);
    crate::output::event("session", json!({"assessment": assessment, "samples": samples}));
    Ok((assessment, samples))
}

pub fn command(env: &Environment) -> Result<(), String> {
    let (assessment, samples) = observe(env, Duration::from_secs(30))?;
    crate::output::data(json!({"assessment": assessment, "samples": samples}));
    for v in &assessment.instances {
        crate::output::line(format_args!("conta {}: {:?}, membros {:?}, host 0x{:x?} convidado {:?}, enviados +{} falhas +{} recebidos +{}",
            v.instance, v.role, v.members.iter().map(|m| format!("{}{}", m.steam_id, if m.host { "(host)" } else { "" })).collect::<Vec<_>>(),
            v.host_state, v.guest_state, v.sent, v.failed, v.received));
    }
    crate::output::line(format_args!("p2pSessionVerified: {:?}; problemas: {:?}", assessment.p2p_session_verified, assessment.problems));
    match assessment.p2p_session_verified {
        Some(_) => Ok(()),
        None => { crate::output::outcome("inconclusive"); Err(format!("session_unverified: {}", assessment.problems.join("; "))) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMUEL: &str = "011000010afd1a3a";
    const CHICO: &str = "0110000140d6d6d1";

    fn block(sent: u64, received: u64, me: &str, session: Option<&str>) -> String {
        format!("13:30:30.100  === canal 7: polls=4657 estranhos=0 enviados={sent} falhas=0 recebidos={received} recusados=0 eu={me} ===\n{}    local ha 7 ms: papel 0, registro mapa 0a1f0000 tipo 0 id 00007ba7\n    do host: nada recebido\n",
            session.map(|s| format!("    sessao 00007FFFFE5BFA00 vista ha 3 ms: 2 membros: {s}\n")).unwrap_or_default())
    }

    #[test]
    fn the_channel_status_is_read_whole_or_not_at_all() {
        let text = block(22, 0, SAMUEL, Some(&format!("{SAMUEL} (host) {CHICO}")));
        let channel = parse_channel(&text).unwrap().unwrap();
        assert_eq!((channel.polls, channel.sent, channel.received), (4657, 22, 0));
        assert_eq!(channel.me.as_deref(), Some("76561198144625210"));
        assert_eq!(channel.sessions[0].members, vec![Member { steam_id: "76561198144625210".into(), host: true },
            Member { steam_id: "76561199048087249".into(), host: false }]);
        assert_eq!(channel.sessions[0].address, "0x00007ffffe5bfa00");
        assert_eq!(channel.local.as_ref().unwrap().id, "00007ba7");
        // Without the closing `do host` line, the block is still arriving.
        assert_eq!(parse_channel(&text[..text.find("    do host").unwrap()]).unwrap(), None);
        // Outside a session the hook has not learned its own account yet (measured 14/09).
        let outside = "13:00:00.000  === canal 7: polls=0 estranhos=0 enviados=0 falhas=0 recebidos=0 recusados=0 eu=0000000000000000 ===\n    local: nada publicado\n    do host: nada recebido\n";
        let channel = parse_channel(outside).unwrap().unwrap();
        assert!(channel.sessions.is_empty() && channel.local.is_none() && channel.me.is_none());
        // Other lines the hook writes around it do not confuse the block.
        let noisy = format!("13:29:59.000  sessao 00007FFFFE5BFA00: 2 membros: {SAMUEL} (host) {CHICO} (eu {SAMUEL})\n{text}");
        assert_eq!(parse_channel(&noisy).unwrap().unwrap().sent, 22);
        assert!(parse_channel("13:00:00.000  === canal 7: polls=x ===\n    do host: nada recebido\n").is_err());
    }

    fn sample(instance: u8, me: &str, first: (u64, u64), second: (u64, u64), session: Option<&str>, controllers: Controllers) -> Sample {
        Sample { instance, configured_id: decimal(me), elapsed_ms: 2600, controllers,
            first: parse_channel(&block(first.0, first.1, me, session)).unwrap().unwrap(),
            second: parse_channel(&block(second.0, second.1, me, session)).unwrap().unwrap() }
    }

    fn live(host_state: i32, guest_state: i32) -> (Controllers, Controllers) {
        (Controllers { host: vec![Controller { address: "0x1".into(), state: host_state }], guest: vec![] },
         Controllers { host: vec![], guest: vec![Controller { address: "0x2".into(), state: guest_state }] })
    }

    #[test]
    fn verified_needs_the_objects_and_the_data() {
        let members = format!("{SAMUEL} (host) {CHICO}");
        let (host, guest) = live(0x10, 7);
        let both = [sample(1, SAMUEL, (20, 0), (22, 0), Some(&members), host.clone()), sample(2, CHICO, (0, 20), (0, 21), Some(&members), guest.clone())];
        let verdict = assess(&both);
        assert_eq!(verdict.p2p_session_verified, Some(true), "{:?}", verdict.problems);
        assert_eq!(verdict.instances[0].role, Role::Host);
        assert_eq!(verdict.instances[1].role, Role::Guest);

        // The objects alone — the host controller outlives the session.
        let stalled = [sample(1, SAMUEL, (22, 0), (22, 0), Some(&members), host.clone()), sample(2, CHICO, (0, 21), (0, 21), Some(&members), guest.clone())];
        let verdict = assess(&stalled);
        assert_eq!(verdict.p2p_session_verified, None);
        assert!(verdict.objects_agree);
        assert!(verdict.problems.iter().any(|p| p.starts_with("session_objects_only")), "{:?}", verdict.problems);

        // Data flowing without the guest's controller in state 7 is not enough either.
        let (_, joining) = live(0x10, 4);
        let verdict = assess(&[sample(1, SAMUEL, (20, 0), (22, 0), Some(&members), host.clone()), sample(2, CHICO, (0, 20), (0, 21), Some(&members), joining)]);
        assert_eq!(verdict.p2p_session_verified, None);

        // One sample is not two: counters cannot move in no time.
        let mut close = both.clone();
        close[1].elapsed_ms = 300;
        assert_eq!(assess(&close).p2p_session_verified, None);

        // Instance 1 cannot stand in for instance 2.
        assert_eq!(assess(&both[..1]).p2p_session_verified, None);

        // A configured account that is not the one running.
        let mut wrong = both.clone();
        wrong[1].configured_id = Some("76561198000000001".into());
        assert_eq!(assess(&wrong).p2p_session_verified, None);
    }

    #[test]
    fn no_session_is_false_only_when_both_sides_say_so() {
        let none = Controllers::default();
        let quiet = [sample(1, SAMUEL, (0, 0), (0, 0), None, none.clone()), sample(2, CHICO, (0, 0), (0, 0), None, none.clone())];
        assert_eq!(assess(&quiet).p2p_session_verified, Some(false));
        // The same, as the hook really reports it before any session: no account of its own.
        let unknown = [sample(1, "0000000000000000", (0, 0), (0, 0), None, none.clone()), sample(2, "0000000000000000", (0, 0), (0, 0), None, none.clone())];
        let mut unknown = unknown;
        unknown[0].configured_id = Some("76561198144625210".into());
        unknown[1].configured_id = Some("76561199048087249".into());
        let verdict = assess(&unknown);
        assert_eq!(verdict.p2p_session_verified, Some(false), "{:?}", verdict.problems);
        // A lingering host controller without a channel session is not "no session".
        let (host, _) = live(0x10, 7);
        let lingering = [sample(1, SAMUEL, (0, 0), (0, 0), None, host), sample(2, CHICO, (0, 0), (0, 0), None, none)];
        let verdict = assess(&lingering);
        assert_eq!(verdict.p2p_session_verified, None);
        assert_eq!(verdict.instances[0].role, Role::Unknown);
        // A controller in another state is a leftover, not an active session.
        let (old, _) = live(2, 7);
        assert_eq!(assess(&[sample(1, SAMUEL, (0, 0), (0, 0), None, old), sample(2, CHICO, (0, 0), (0, 0), None, Controllers::default())])
            .p2p_session_verified, Some(false));
    }

    #[test]
    fn a_third_member_is_outside_what_this_machine_can_prove() {
        let members = format!("{SAMUEL} (host) {CHICO} 0110000100000001");
        let (host, guest) = live(0x10, 7);
        let verdict = assess(&[sample(1, SAMUEL, (20, 0), (24, 0), Some(&members), host), sample(2, CHICO, (0, 20), (0, 22), Some(&members), guest)]);
        assert_eq!(verdict.p2p_session_verified, None);
        assert!(verdict.problems.iter().any(|p| p.starts_with("not_our_pair")));
    }
}
