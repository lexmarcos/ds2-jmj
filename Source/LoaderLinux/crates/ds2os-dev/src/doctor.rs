//! Exercising the chain of proof, not only listing what is installed.
//!
//! The old `doctor` said "tudo pronto" through the whole of 13/09 while every
//! `game enter` failed: it confirmed that files existed, and the two faults
//! lived one step further along — the build check read an executable from the
//! wrong directory, and the API spelled the account in another notation. So
//! every link a test depends on is exercised here with whatever is running:
//! the executable the probe checks, a real MemProbe round trip, the hook
//! receipt of the running boot against the config on disk, and the accounts
//! against what the server actually lists.
//!
//! Each check is `ok`, `warning`, `problem` or `skipped`. A check that could
//! not run is `skipped`, never `ok`; only a `problem` fails the command.

use std::collections::BTreeMap;
use std::path::Path;
use std::time::Duration;

use serde::Serialize;
use serde_json::{json, Value};

use crate::env::{Environment, Install};
use crate::observe::{self, Build};
use crate::probe::{self, Located, Reason, Where};
use crate::settings::HarnessConfig;
use crate::{api, game, pad, proc, server};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Status { Ok, Warning, Problem, Skipped }

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Check {
    pub name: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub instance: Option<u8>,
    pub status: Status,
    pub detail: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub fix: Option<String>,
    #[serde(skip_serializing_if = "Value::is_null")]
    pub data: Value,
}

impl Check {
    fn new(name: &'static str, instance: Option<u8>, status: Status, detail: impl Into<String>) -> Self {
        Self { name, instance, status, detail: detail.into(), fix: None, data: Value::Null }
    }
    fn fix(mut self, fix: impl Into<String>) -> Self { self.fix = Some(fix.into()); self }
    fn data(mut self, data: Value) -> Self { self.data = data; self }
}

/// Every check, gathered from the machine as it is now.
pub fn checks(env: &Environment) -> Vec<Check> {
    let mut checks: Vec<Check> = env.problems().into_iter()
        .map(|p| Check::new("environment", None, Status::Problem, p.what).fix(p.fix)).collect();
    let settings = HarnessConfig::load();

    let server_pid = server::status(env).pid;
    let listed = match (&env.server, server_pid) {
        (Some(server), Some(_)) => Some(api::players_until(server, api::web_port(&server.config), Duration::from_secs(5))),
        _ => None,
    };
    checks.push(server_api(env.server.is_some(), server_pid.is_some(), listed.as_ref()));
    checks.extend(distinct_identities(&settings.steam_ids));

    let every_game = proc::game_pids();
    let mut owned = Vec::new();
    for account in [1u8, 2] {
        checks.push(identity(account, settings.steam_ids.get(&account).map(String::as_str)));
        let Some(install) = env.installs.iter().find(|i| i.account == account) else {
            checks.push(Check::new("install", Some(account), Status::Problem, "instalação não resolvida")
                .fix(if account == 2 { "configure a segunda Steam: `ds2os-dev steam2 init`" } else { "rode `ds2os-dev doctor` sem --json para ver o ambiente" }));
            continue;
        };
        checks.push(game_exe(install));
        checks.push(injector_installed(install, env.injector_source.as_deref()));
        checks.push(logs_size(install));

        let in_prefix = game::compat_data(env, account).map(|p| game::instance_pids(&p)).unwrap_or_default();
        owned.extend(in_prefix.iter().copied());
        let games: Vec<u32> = every_game.iter().copied().filter(|pid| in_prefix.contains(pid)).collect();
        checks.push(game_processes(account, &games));
        if games.is_empty() {
            for name in ["memprobe", "receipt", "config_drift", "api_identity"] {
                checks.push(Check::new(name, Some(account), Status::Skipped, "instância parada"));
            }
            continue;
        }

        let located = probe::locate(install, Duration::from_secs(3));
        checks.push(memprobe(&located));
        let dir = install.game_dir.as_path();
        let receipt = read_json(&dir.join("DS2_Harness.json"));
        if located.state == Where::Unknown {
            checks.push(Check::new("receipt", Some(account), Status::Skipped, "o jogo não respondeu ao MemProbe"));
            checks.push(Check::new("config_drift", Some(account), Status::Skipped, "o jogo não respondeu ao MemProbe"));
        } else {
            let boot = observe::sample(dir).and_then(|(_, boot)| boot);
            let receipt_check = receipt_of_boot(account, boot.as_deref(), receipt.as_ref());
            let valid = receipt_check.status == Status::Ok || receipt_check.data.get("sameBoot") == Some(&json!(true));
            checks.push(receipt_check);
            checks.push(match (valid, receipt.as_ref().and_then(|r| r.get("configured"))) {
                (true, Some(configured)) => config_drift(account, configured, read_json(&dir.join("Injector.config")).as_ref()),
                _ => Check::new("config_drift", Some(account), Status::Skipped, "sem recibo deste boot"),
            });
        }
        checks.push(api_identity(account, settings.steam_ids.get(&account).map(String::as_str), listed.as_ref(), located.state));
    }

    checks.push(extra_game_processes(&every_game, &owned));
    checks.push(pad_check(pad::running(1), !every_game.is_empty()));
    checks.push(wine_orphans(&every_game));
    checks.push(x11_clients());
    checks
}

fn read_json(path: &Path) -> Option<Value> { serde_json::from_slice(&std::fs::read(path).ok()?).ok() }

fn server_api(built: bool, running: bool, listed: Option<&Result<Vec<api::Player>, String>>) -> Check {
    const NAME: &str = "server_api";
    match listed {
        _ if !built => Check::new(NAME, None, Status::Skipped, "servidor não compilado"),
        _ if !running => Check::new(NAME, None, Status::Skipped, "servidor parado").fix("`ds2os-dev server up`"),
        None => Check::new(NAME, None, Status::Skipped, "servidor parado"),
        Some(Err(e)) => Check::new(NAME, None, Status::Problem, format!("a API não respondeu: {e}"))
            .fix("confira WebUIServerUsername/WebUIServerPassword no config do servidor e `ds2os-dev logs server`"),
        Some(Ok(players)) => {
            let ids: Vec<&str> = players.iter().map(|p| p.steam_id.as_str()).collect();
            // The harness compares these with the configured decimal IDs; an
            // entry in another notation matches nobody, as on 13/09.
            let odd: Vec<&str> = ids.iter().copied().filter(|id| !steam_id64(id)).collect();
            if odd.is_empty() {
                Check::new(NAME, None, Status::Ok, format!("{} jogador(es) listados", players.len())).data(json!({"steamIds": ids}))
            } else {
                Check::new(NAME, None, Status::Problem, format!("a API listou IDs fora do formato decimal de 17 dígitos: {}", odd.join(", ")))
                    .data(json!({"steamIds": ids}))
            }
        }
    }
}

/// A SteamID64 for an individual account, in decimal.
fn steam_id64(id: &str) -> bool {
    id.len() == 17 && id.bytes().all(|b| b.is_ascii_digit()) && id.starts_with("7656119")
}

fn identity(account: u8, id: Option<&str>) -> Check {
    match id {
        None => Check::new("identity", Some(account), Status::Problem, "Steam ID não configurado")
            .fix(format!("`ds2os-dev game identity --instance {account} <SteamID64 decimal>`")),
        Some(id) if !steam_id64(id) => Check::new("identity", Some(account), Status::Problem,
            format!("{id} não é um SteamID64 decimal de 17 dígitos"))
            .fix(format!("`ds2os-dev game identity --instance {account} <SteamID64 decimal>`")),
        Some(id) => Check::new("identity", Some(account), Status::Ok, id),
    }
}

fn distinct_identities(ids: &BTreeMap<u8, String>) -> Option<Check> {
    let (Some(one), Some(two)) = (ids.get(&1), ids.get(&2)) else { return None; };
    Some(if one == two {
        Check::new("identities_distinct", None, Status::Problem, format!("as duas instâncias usam {one}"))
            .fix("cada instância precisa da própria conta Steam")
    } else {
        Check::new("identities_distinct", None, Status::Ok, "contas diferentes")
    })
}

fn game_exe(install: &Install) -> Check {
    let account = Some(install.account);
    let path = install.game_exe.as_ref().map(|p| p.display().to_string()).unwrap_or_else(|| "nenhum".into());
    match observe::build_of(install, ds2os_core::exe::DS2_SOTFS_1_03) {
        Build::Expected => Check::new("game_exe", account, Status::Ok, format!("{path} é o 1.03 / Calibrations 2.02")),
        Build::Other => Check::new("game_exe", account, Status::Problem, format!("{path} não é o 1.03 / Calibrations 2.02"))
            .fix("verifique os arquivos do jogo pela Steam; todo endereço do injector é desta versão"),
        Build::Missing => Check::new("game_exe", account, Status::Problem, format!("executável ausente ou ilegível: {path}")),
    }
}

fn sha256(path: &Path) -> Option<String> {
    crate::output::fingerprint(path).get("sha256").and_then(Value::as_str).map(str::to_owned)
}

fn injector_installed(install: &Install, source: Option<&Path>) -> Check {
    let account = Some(install.account);
    let installed = install.game_dir.join("Injector.dll");
    let Some(source) = source.map(|s| s.join("Injector.dll")) else {
        return Check::new("injector_installed", account, Status::Skipped, "sem Injector.dll de origem para comparar");
    };
    match (sha256(&installed), sha256(&source)) {
        (None, _) => Check::new("injector_installed", account, Status::Problem, "Injector.dll ausente na instalação")
            .fix("`ds2os-dev game prepare`"),
        (Some(_), None) => Check::new("injector_installed", account, Status::Skipped, format!("{} ilegível", source.display())),
        (Some(a), Some(b)) if a == b => Check::new("injector_installed", account, Status::Ok, format!("igual a {}", source.display()))
            .data(json!({"sha256": a})),
        (Some(a), Some(b)) => Check::new("injector_installed", account, Status::Warning,
            format!("a DLL instalada difere de {}", source.display()))
            .fix("`ds2os-dev game prepare` com as mesmas flags, e relance o jogo").data(json!({"installed": a, "source": b})),
    }
}

/// The injector's own logs are never rotated; a day of the timer patch alone
/// reached 412 MB.
fn logs_size(install: &Install) -> Check {
    const LARGE: u64 = 256 << 20;
    let mut sizes: Vec<(String, u64)> = std::fs::read_dir(&install.game_dir).into_iter().flatten().flatten()
        .filter_map(|entry| {
            let name = entry.file_name().to_string_lossy().into_owned();
            (name.starts_with("DS2") && name.ends_with(".log")).then(|| Some((name, entry.metadata().ok()?.len())))?
        }).collect();
    sizes.sort_by(|a, b| b.1.cmp(&a.1));
    let total: u64 = sizes.iter().map(|(_, size)| size).sum();
    let large: Vec<String> = sizes.iter().filter(|(_, size)| *size > LARGE).map(|(name, size)| format!("{name} {} MB", size >> 20)).collect();
    let data = json!({"totalBytes": total, "largest": sizes.iter().take(3).map(|(n, s)| json!({"name": n, "bytes": s})).collect::<Vec<_>>()});
    if large.is_empty() {
        Check::new("logs_size", Some(install.account), Status::Ok, format!("{} MB de logs", total >> 20)).data(data)
    } else {
        Check::new("logs_size", Some(install.account), Status::Warning, format!("logs grandes: {}", large.join(", ")))
            .fix("com o jogo fechado, esvazie-os (`: > <arquivo>`)").data(data)
    }
}

/// Two games on one account break every session, and the symptom points
/// somewhere else.
fn game_processes(account: u8, games: &[u32]) -> Check {
    match games.len() {
        0 => Check::new("game_processes", Some(account), Status::Skipped, "instância parada"),
        1 => Check::new("game_processes", Some(account), Status::Ok, format!("pid {}", games[0])),
        _ => Check::new("game_processes", Some(account), Status::Problem, format!("{} jogos no mesmo prefixo: {games:?}", games.len()))
            .fix(format!("`ds2os-dev game stop --instance {account}` e lance de novo")),
    }
}

fn memprobe(located: &Located) -> Check {
    let account = Some(located.instance);
    let data = json!({"state": located.state, "reason": located.reason, "latencyMs": located.elapsed_ms});
    let detail = format!("{} ({}) em {} ms", located.state, located.reason, located.elapsed_ms);
    match (located.state, located.reason) {
        // `doctor` runs beside controllers; a held lock says nothing about the game.
        (Where::Unknown, Reason::ProbeBusy) => Check::new("memprobe", account, Status::Skipped, detail).data(data),
        (Where::Unknown, Reason::RequestNotConsumed) => Check::new("memprobe", account, Status::Problem, detail)
            .fix("o jogo ainda está iniciando, ou roda sem o injector com a sonda: espere o título ou relance").data(data),
        (Where::Unknown, _) => Check::new("memprobe", account, Status::Problem, detail).data(data),
        _ if located.elapsed_ms > 1500 => Check::new("memprobe", account, Status::Warning, format!("{detail}, lento")).data(data),
        _ => Check::new("memprobe", account, Status::Ok, detail).data(data),
    }
}

/// The receipt proves which hooks the running boot installed, and only for
/// that boot: an old DLL, or a receipt left by an earlier launch, proves
/// nothing about the process that is open now.
fn receipt_of_boot(account: u8, boot: Option<&str>, receipt: Option<&Value>) -> Check {
    const NAME: &str = "receipt";
    let relaunch = format!("instale a DLL atual (`ds2os-dev game prepare`) e relance a instância {account}");
    let Some(boot) = boot else {
        return Check::new(NAME, Some(account), Status::Problem, "DS2_Nav.txt sem bootId recente: DLL antiga ou telemetria parada").fix(relaunch);
    };
    let Some(receipt) = receipt else {
        return Check::new(NAME, Some(account), Status::Problem, "DS2_Harness.json ausente ou ilegível").fix(relaunch);
    };
    let receipt_boot = receipt.get("bootId").and_then(Value::as_str).unwrap_or_default();
    if receipt.get("schemaVersion").and_then(Value::as_u64) != Some(1) || receipt_boot != boot {
        return Check::new(NAME, Some(account), Status::Problem, format!("recibo do boot {receipt_boot:?}, o jogo está no boot {boot}")).fix(relaunch);
    }
    let hooks = receipt.get("hooks").and_then(Value::as_object);
    let failed: Vec<&str> = hooks.into_iter().flatten().filter(|(_, ok)| ok.as_bool() != Some(true)).map(|(name, _)| name.as_str()).collect();
    let count = hooks.map_or(0, |h| h.len());
    if count == 0 || !failed.is_empty() {
        return Check::new(NAME, Some(account), Status::Problem, format!("hooks que não instalaram: {}",
            if count == 0 { "nenhum hook no recibo".into() } else { failed.join(", ") }))
            .data(json!({"sameBoot": true, "bootId": boot}));
    }
    Check::new(NAME, Some(account), Status::Ok, format!("{count} hooks instalados no boot {boot}")).data(json!({"sameBoot": true, "bootId": boot}))
}

/// What the running boot was configured with, against what `Injector.config`
/// says now. The file is read at injection, so a change after the launch —
/// `up` rewrites it from its own flags — is not in the game that is open.
fn config_drift(account: u8, configured: &Value, config: Option<&Value>) -> Check {
    const PAIRS: [(&str, &str); 5] = [("autoRematch", "DS2AutoRematch"), ("forceZone", "DS2ForceMultiPlayZone"),
        ("removeFog", "DS2RemovePhantomFog"), ("seamless", "DS2SeamlessCoop"), ("timer", "DS2PatchPhantomTimers")];
    let Some(config) = config else {
        return Check::new("config_drift", Some(account), Status::Problem, "Injector.config ausente ou ilegível").fix("`ds2os-dev game prepare`");
    };
    let differences: Vec<String> = PAIRS.iter().filter_map(|(running, file)| {
        let running_value = configured.get(*running).and_then(Value::as_bool).unwrap_or(false);
        let file_value = config.get(*file).and_then(Value::as_bool).unwrap_or(false);
        (running_value != file_value).then(|| format!("{running}: jogo {running_value}, arquivo {file_value}"))
    }).collect();
    if differences.is_empty() {
        Check::new("config_drift", Some(account), Status::Ok, "o jogo roda com a config do arquivo").data(configured.clone())
    } else {
        Check::new("config_drift", Some(account), Status::Problem, differences.join("; "))
            .fix(format!("relance a instância {account} para aplicar o Injector.config, ou reescreva-o com as flags do jogo aberto"))
            .data(json!({"running": configured}))
    }
}

fn api_identity(account: u8, id: Option<&str>, listed: Option<&Result<Vec<api::Player>, String>>, state: Where) -> Check {
    const NAME: &str = "api_identity";
    let (Some(id), Some(Ok(players))) = (id, listed) else {
        return Check::new(NAME, Some(account), Status::Skipped, "sem Steam ID configurado ou sem API");
    };
    let ids: Vec<&str> = players.iter().map(|p| p.steam_id.as_str()).collect();
    match players.iter().find(|p| p.steam_id == id) {
        Some(player) => Check::new(NAME, Some(account), Status::Ok, format!("{id} conectado como {:?}", player.name)),
        None if state == Where::World => Check::new(NAME, Some(account), Status::Problem,
            format!("no mundo, mas a API não lista {id}"))
            .fix("o jogo entrou offline ou com outra conta: `ds2os-dev game leave` e `game enter`")
            .data(json!({"listed": ids})),
        None => Check::new(NAME, Some(account), Status::Warning, format!("{state}: a API ainda não lista {id}")).data(json!({"listed": ids})),
    }
}

fn extra_game_processes(every_game: &[u32], owned: &[u32]) -> Check {
    let extra: Vec<u32> = every_game.iter().copied().filter(|pid| !owned.contains(pid)).collect();
    if extra.is_empty() {
        return Check::new("extra_game_processes", None, Status::Ok, "nenhum jogo fora dos prefixos das instâncias");
    }
    let prefixes: Vec<Value> = extra.iter().map(|pid| json!({"pid": pid, "winePrefix": proc::env_of(*pid, "WINEPREFIX")})).collect();
    Check::new("extra_game_processes", None, Status::Problem, format!("jogo rodando fora das instâncias: {extra:?}"))
        .fix("feche-o; um segundo jogo numa conta quebra toda sessão").data(json!(prefixes))
}

fn pad_check(running: bool, games: bool) -> Check {
    match (running, games) {
        (true, _) => Check::new("pad", None, Status::Ok, "pad no ar"),
        (false, true) => Check::new("pad", None, Status::Warning, "jogo aberto sem o pad: o jogo só enumera controles ao iniciar")
            .fix("`ds2os-dev pad start` e relance o jogo"),
        (false, false) => Check::new("pad", None, Status::Skipped, "pad parado; `up` o inicia antes dos jogos"),
    }
}

/// Every Wine prefix leaves `xalia.exe` and `winedevice.exe` behind when a game
/// stops, and they hold X11 connections until the server refuses new ones.
fn wine_orphans(every_game: &[u32]) -> Check {
    let mut orphans: Vec<(u32, String, Option<String>)> = Vec::new();
    let game_prefixes: Vec<String> = every_game.iter().filter_map(|pid| proc::env_of(*pid, "WINEPREFIX"))
        .map(|p| p.trim_end_matches('/').to_owned()).collect();
    for entry in std::fs::read_dir("/proc").into_iter().flatten().flatten() {
        let Ok(pid) = entry.file_name().to_string_lossy().parse::<u32>() else { continue };
        let Ok(comm) = std::fs::read_to_string(format!("/proc/{pid}/comm")) else { continue };
        let comm = comm.trim();
        if comm != "xalia.exe" && comm != "winedevice.exe" { continue; }
        let prefix = proc::env_of(pid, "WINEPREFIX").map(|p| p.trim_end_matches('/').to_owned());
        if prefix.as_ref().is_some_and(|p| game_prefixes.contains(p)) { continue; }
        orphans.push((pid, comm.to_owned(), prefix));
    }
    orphans.sort();
    if orphans.is_empty() {
        return Check::new("wine_orphans", None, Status::Ok, "nenhum xalia.exe/winedevice.exe sem jogo");
    }
    let pids: Vec<String> = orphans.iter().map(|(pid, _, _)| pid.to_string()).collect();
    Check::new("wine_orphans", None, Status::Warning, format!("{} processo(s) do Wine de prefixos sem jogo", orphans.len()))
        .fix(if every_game.is_empty() { "pkill -f xalia.exe ; pkill -f winedevice.exe".to_owned() } else { format!("kill {}", pids.join(" ")) })
        .data(json!(orphans.iter().map(|(pid, name, prefix)| json!({"pid": pid, "name": name, "winePrefix": prefix})).collect::<Vec<_>>()))
}

/// Xorg refuses clients past 256 by default, and the only symptom is
/// `Maximum number of clients reached` from whatever connects next.
fn x11_clients() -> Check {
    let Ok(sockets) = std::fs::read_to_string("/proc/net/unix") else {
        return Check::new("x11_clients", None, Status::Skipped, "/proc/net/unix ilegível");
    };
    let count = sockets.lines().filter(|line| line.contains("/tmp/.X11-unix/X")).count();
    if count >= 200 {
        Check::new("x11_clients", None, Status::Warning, format!("{count} conexões X11; o Xorg recusa acima de 256"))
            .fix("feche os órfãos do Wine (veja wine_orphans)").data(json!({"connections": count}))
    } else {
        Check::new("x11_clients", None, Status::Ok, format!("{count} conexões X11")).data(json!({"connections": count}))
    }
}

pub fn summary(checks: &[Check]) -> Value {
    let count = |status| checks.iter().filter(|c| c.status == status).count();
    json!({"ok": count(Status::Ok), "warning": count(Status::Warning), "problem": count(Status::Problem), "skipped": count(Status::Skipped)})
}

#[cfg(test)]
mod tests {
    use super::*;

    fn player(id: &str) -> api::Player {
        api::Player { steam_id: id.into(), name: "Samuel".into(), player_id: 1, soul_level: 1, souls: None, soul_memory: 1,
            death_count: None, multiplay_count: None, covenant: String::new(), status: String::new(), location: String::new(), play_time: String::new() }
    }

    #[test]
    fn an_account_in_another_notation_is_a_problem() {
        assert_eq!(identity(1, Some("76561198144625210")).status, Status::Ok);
        assert_eq!(identity(1, Some("011000010afd1a3a")).status, Status::Problem);
        assert_eq!(identity(2, None).status, Status::Problem);
        let listed = Ok(vec![player("76561198144625210"), player("011000010afd1a3a")]);
        assert_eq!(server_api(true, true, Some(&listed)).status, Status::Problem);
        assert_eq!(server_api(true, true, Some(&Ok(vec![player("76561198144625210")]))).status, Status::Ok);
        assert_eq!(server_api(true, false, None).status, Status::Skipped);
    }

    #[test]
    fn a_world_the_api_does_not_list_is_a_problem() {
        let listed = Ok(vec![player("76561198144625210")]);
        assert_eq!(api_identity(1, Some("76561198144625210"), Some(&listed), Where::World).status, Status::Ok);
        assert_eq!(api_identity(2, Some("76561199048087249"), Some(&listed), Where::World).status, Status::Problem);
        assert_eq!(api_identity(2, Some("76561199048087249"), Some(&listed), Where::Title).status, Status::Warning);
        assert_eq!(api_identity(2, Some("76561199048087249"), None, Where::World).status, Status::Skipped);
    }

    #[test]
    fn the_same_account_twice_is_refused() {
        let mut ids = BTreeMap::new();
        assert!(distinct_identities(&ids).is_none());
        ids.insert(1, "76561198144625210".to_owned());
        ids.insert(2, "76561198144625210".to_owned());
        assert_eq!(distinct_identities(&ids).unwrap().status, Status::Problem);
        ids.insert(2, "76561199048087249".to_owned());
        assert_eq!(distinct_identities(&ids).unwrap().status, Status::Ok);
    }

    #[test]
    fn a_receipt_only_proves_the_boot_that_wrote_it() {
        let receipt = json!({"schemaVersion": 1, "bootId": "1640-1", "hooks": {"DS2 Nav": true, "DS2 Seamless Coop": true}});
        assert_eq!(receipt_of_boot(1, Some("1640-1"), Some(&receipt)).status, Status::Ok);
        assert_eq!(receipt_of_boot(1, Some("2000-9"), Some(&receipt)).status, Status::Problem);
        assert_eq!(receipt_of_boot(1, None, Some(&receipt)).status, Status::Problem);
        assert_eq!(receipt_of_boot(1, Some("1640-1"), None).status, Status::Problem);
        let failed = json!({"schemaVersion": 1, "bootId": "1640-1", "hooks": {"DS2 Nav": true, "DS2 Seamless Coop": false}});
        let check = receipt_of_boot(1, Some("1640-1"), Some(&failed));
        assert_eq!(check.status, Status::Problem);
        assert!(check.detail.contains("DS2 Seamless Coop"), "{}", check.detail);
        assert_eq!(check.data["sameBoot"], true);
    }

    #[test]
    fn a_config_changed_after_the_launch_is_drift() {
        let configured = json!({"autoRematch": false, "forceZone": true, "removeFog": false, "seamless": true, "timer": true});
        let file = json!({"DS2AutoRematch": false, "DS2ForceMultiPlayZone": true, "DS2RemovePhantomFog": false,
            "DS2SeamlessCoop": true, "DS2PatchPhantomTimers": true, "ServerPort": 50050});
        assert_eq!(config_drift(1, &configured, Some(&file)).status, Status::Ok);
        let mut rewritten = file.clone();
        rewritten["DS2SeamlessCoop"] = json!(false);
        rewritten["DS2RemovePhantomFog"] = json!(true);
        let check = config_drift(1, &configured, Some(&rewritten));
        assert_eq!(check.status, Status::Problem);
        assert_eq!(check.detail, "removeFog: jogo false, arquivo true; seamless: jogo true, arquivo false");
        assert_eq!(config_drift(1, &configured, None).status, Status::Problem);
    }

    #[test]
    fn a_probe_that_could_not_run_is_never_ok() {
        let mut located = Located::unasked(1, None, Reason::ProbeBusy);
        assert_eq!(memprobe(&located).status, Status::Skipped);
        located.reason = Reason::RequestNotConsumed;
        assert_eq!(memprobe(&located).status, Status::Problem);
        (located.state, located.reason, located.elapsed_ms) = (Where::Title, Reason::TitleFlagSet, 520);
        assert_eq!(memprobe(&located).status, Status::Ok);
        located.elapsed_ms = 2400;
        assert_eq!(memprobe(&located).status, Status::Warning);
    }

    #[test]
    fn processes_are_counted_per_account_and_outside_them() {
        assert_eq!(game_processes(1, &[]).status, Status::Skipped);
        assert_eq!(game_processes(1, &[10]).status, Status::Ok);
        assert_eq!(game_processes(1, &[10, 11]).status, Status::Problem);
        assert_eq!(extra_game_processes(&[10, 20], &[10, 20, 30]).status, Status::Ok);
        let extra = extra_game_processes(&[10, 20, 40], &[10, 20]);
        assert_eq!(extra.status, Status::Problem);
        assert!(extra.detail.contains("[40]"), "{}", extra.detail);
    }
}
