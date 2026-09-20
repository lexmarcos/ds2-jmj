//! Menu actions with per-instance identity, fresh state and a single deadline.
use std::time::{Duration, Instant};
use serde_json::json;
use crate::{api, control::Deadline, env::Environment, game, observe, output, pad, probe::{self, Located, Reason, Where}, screen::{self, GameWindow}};

pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(180);
#[derive(Debug, Clone, serde::Serialize)]
pub struct Arrival { pub character: String, pub seconds: f64 }

/// What a menu walk had seen when it stopped. A bare `timeout` cost a day on
/// 13/09: it could not tell a game that never answered from a button that did
/// nothing, so the error carries the last answer and how far the walk got.
#[derive(Default)]
struct Progress { window: bool, last: Option<Located>, presses: u32, missing_from_api: u32 }

impl Progress {
    fn saw(&mut self, located: Located) -> Where {
        let state = located.state;
        self.last = Some(located);
        state
    }

    fn explain(&self, error: String) -> String {
        if !error.starts_with("timeout:") && !error.starts_with("leave_failed:") { return error; }
        let last = match &self.last {
            Some(l) => format!("último estado {} ({}) há {:.1}s", l.state, l.reason, l.elapsed_ms as f64 / 1000.0),
            None if !self.window => "a janela da instância não apareceu".into(),
            None => "nenhum estado lido".into(),
        };
        format!("{error}; {last}; {} tecla(s); {} leitura(s) da API sem a conta", self.presses, self.missing_from_api)
    }
}

/// An unresolved account is never substituted with another window.
pub fn window_for(environment: &Environment, account: u8) -> Result<GameWindow, String> {
    let prefix = game::compat_data(environment, account)?;
    let pids = game::instance_pids(&prefix);
    screen::windows()?.into_iter().find(|w| w.pid.is_some_and(|p| pids.contains(&p)))
        .ok_or_else(|| format!("instance_unresolved: conta {account} sem janela pertencente ao seu prefixo"))
}

fn wait_for_window(env: &Environment, account: u8, deadline: Deadline, progress: &mut Progress) -> Result<GameWindow, String> {
    loop {
        deadline.remaining()?;
        if let Ok(w) = window_for(env, account) { progress.window = true; return Ok(w); }
        deadline.sleep(Duration::from_millis(300))?;
    }
}

pub fn wait_title(env: &Environment, account: u8, timeout: Duration) -> Result<(), String> {
    let deadline = Deadline::after(timeout);
    let mut progress = Progress::default();
    let result = (|| {
        wait_for_window(env, account, deadline, &mut progress)?;
        loop {
            deadline.remaining()?;
            match progress.saw(locate_until(env, account, deadline)) {
                Where::Title => return Ok(()),
                Where::World => return Err("already_in_world: --no-enter exige a instância no título".into()),
                _ => deadline.sleep(Duration::from_millis(300))?,
            }
        }
    })();
    result.map_err(|e| progress.explain(e))
}

pub fn locate(env: &Environment, account: u8) -> Located {
    locate_until(env, account, Deadline::after(Duration::from_secs(4)))
}
fn locate_until(env: &Environment, account: u8, deadline: Deadline) -> Located {
    let Some(install) = env.installs.iter().find(|i| i.account == account) else {
        return Located::unasked(account, None, Reason::InstanceMissing);
    };
    if observe::processes(env, account).is_empty() {
        return Located::unasked(account, install.game_exe.clone(), Reason::InstanceStopped);
    }
    match deadline.remaining() {
        Ok(left) => probe::locate(install, left.min(Duration::from_secs(2))),
        Err(e) => Located::unasked(account, install.game_exe.clone(),
            if e.starts_with("cancelled") { Reason::Cancelled } else { Reason::Timeout }),
    }
}

pub fn press(env: &Environment, account: u8, command: &str, deadline: Deadline) -> Result<(), String> {
    deadline.remaining()?;
    let result = (|| {
        let window = window_for(env, account)?;
        screen::focus(&window)?;
        pad::send_until(1, command, deadline.remaining()?).map(|_| ())
    })();
    output::event("press", json!({"instance": account, "command": command, "ok": result.is_ok(), "error": result.as_ref().err()}));
    result
}

pub fn enter(env: &Environment, instance: u8, expect: Option<&str>, timeout: Duration) -> Result<Arrival, String> {
    let mut progress = Progress::default();
    let result = enter_with(env, instance, expect, Deadline::after(timeout), &mut progress);
    result.map_err(|e| progress.explain(e))
}

fn enter_with(env: &Environment, instance: u8, expect: Option<&str>, deadline: Deadline, progress: &mut Progress) -> Result<Arrival, String> {
    let started = Instant::now();
    let id = observe::steam_id(instance)?;
    let server = env.server.as_ref().ok_or("api_unavailable: servidor ausente")?;
    // Check identity/API before sending any input.
    api::players_until(server, api::web_port(&server.config), deadline.remaining()?)?;
    if !pad::running(1) { return Err("pad_unavailable: inicie o pad antes do jogo".into()); }
    wait_for_window(env, instance, deadline, progress)?;
    let identity = observe::processes(env, instance);
    let mut offline_since: Option<Instant> = None;
    let mut recoveries = 0;
    loop {
        deadline.remaining()?;
        if observe::processes(env, instance) != identity { return Err("process_changed: a instância reiniciou durante enter".into()); }
        match progress.saw(locate_until(env, instance, deadline)) {
            Where::World => {
                let list = api::players_until(server, api::web_port(&server.config), deadline.remaining()?)?;
                if let Some(player) = observe::player_for(&list, &id)? {
                    if player.name.is_empty() { deadline.sleep(Duration::from_millis(300))?; continue; }
                    if let Some(expected) = expect {
                        if !player.name.eq_ignore_ascii_case(expected) {
                            return Err(format!("wrong_character: carregou {} e não {expected}", player.name));
                        }
                    }
                    apply_death_profile(env, instance, deadline)?;
                    return Ok(Arrival { character: player.name.clone(), seconds: started.elapsed().as_secs_f64() });
                }
                // The second bug of 13/09 lived here: the API listed the
                // account in another notation, and this branch waited in
                // silence for a player the server was already showing.
                progress.missing_from_api += 1;
                let since = offline_since.get_or_insert_with(Instant::now);
                output::event("enter", json!({"instance": instance, "phase": "world_without_api_record", "expected": id,
                    "listed": list.iter().map(|p| &p.steam_id).collect::<Vec<_>>(), "offlineForMs": since.elapsed().as_millis()}));
                if since.elapsed() > Duration::from_secs(45) {
                    if recoveries >= 2 { return Err("offline: entrou sem a conexão Steam esperada".into()); }
                    recoveries += 1;
                    output::event("enter", json!({"instance": instance, "phase": "offline_recovery", "attempt": recoveries}));
                    leave_until(env, instance, deadline, false)?;
                    offline_since = None;
                }
            }
            Where::Title => {
                offline_since = None;
                press(env, instance, if progress.presses % 2 == 0 { "press start" } else { "press a" }, deadline)?;
                progress.presses += 1;
                deadline.sleep(Duration::from_millis(1200))?;
            }
            // Loading is not world; unknown never authorizes blind presses.
            Where::Loading | Where::Unknown => {}
        }
        deadline.sleep(Duration::from_millis(300))?;
    }
}

/// Every launch starts the death hook over in observe mode, so a saved profile
/// goes back on at each arrival, and only counts once the hook echoes it.
fn apply_death_profile(env: &Environment, instance: u8, deadline: Deadline) -> Result<(), String> {
    let Some(profile) = crate::settings::HarnessConfig::load().death_profile else { return Ok(()); };
    let install = env.installs.iter().find(|i| i.account == instance).ok_or("instance_missing: instalação ausente")?;
    let result = crate::death::apply_profile(install, &profile, deadline.remaining()?.min(Duration::from_secs(10)));
    output::event("enter", json!({"instance": instance, "phase": "death_profile", "profile": profile, "ok": result.is_ok(), "error": result.as_ref().err()}));
    result
}

pub fn leave(env: &Environment, instance: u8, timeout: Duration) -> Result<f64, String> {
    let deadline = Deadline::after(timeout);
    let located = locate_until(env, instance, deadline);
    match located.state {
        Where::Title => return Ok(0.0),
        Where::World => {},
        _ => return Err(format!("state_unknown: só é possível sair a partir de um mundo confirmado; o jogo respondeu {} ({})",
            located.state, located.reason)),
    }
    leave_until(env, instance, deadline, false)
}

pub fn leave_now(env: &Environment, instance: u8, timeout: Duration, dismiss_first: bool) -> Result<f64, String> {
    leave_until(env, instance, Deadline::after(timeout), dismiss_first)
}

fn leave_until(env: &Environment, instance: u8, deadline: Deadline, dismiss_first: bool) -> Result<f64, String> {
    let mut progress = Progress { window: true, ..Progress::default() };
    let result = leave_with(env, instance, deadline, dismiss_first, &mut progress);
    result.map_err(|e| progress.explain(e))
}

fn leave_with(env: &Environment, instance: u8, deadline: Deadline, dismiss_first: bool, progress: &mut Progress) -> Result<f64, String> {
    let started = Instant::now();
    let identity = observe::processes(env, instance);
    let walk = [("press start", 900), ("press rb", 250), ("press rb", 250), ("press rb", 250),
        ("press rb", 250), ("press rb", 400), ("dpad down", 250), ("dpad down", 350),
        ("press a", 700), ("dpad left", 350), ("press a", 0)];
    for attempt in 0..2 {
        if attempt > 0 || dismiss_first {
            press(env, instance, "press a", deadline)?;
            progress.presses += 1;
            deadline.sleep(Duration::from_millis(900))?;
        }
        for (command, gap) in walk {
            if observe::processes(env, instance) != identity { return Err("process_changed: instância reiniciou durante leave".into()); }
            press(env, instance, command, deadline)?;
            progress.presses += 1;
            if gap > 0 { deadline.sleep(Duration::from_millis(gap))?; }
        }
        let until = Instant::now() + Duration::from_secs(20);
        while Instant::now() < until {
            deadline.remaining()?;
            if progress.saw(locate_until(env, instance, deadline)) == Where::Title { return Ok(started.elapsed().as_secs_f64()); }
            deadline.sleep(Duration::from_millis(300))?;
        }
    }
    Err("leave_failed: menu não voltou ao título em duas tentativas".into())
}

pub fn open_instances(env: &Environment) -> Vec<u8> {
    [1, 2].into_iter().filter(|i| !observe::processes(env, *i).is_empty()).collect()
}
pub fn expected_character(settings: &crate::settings::HarnessConfig, instance: u8) -> Option<String> {
    settings.characters.get(&instance).cloned()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_timeout_says_where_the_walk_was_stuck() {
        let mut progress = Progress::default();
        assert_eq!(progress.explain("timeout: prazo".into()), "timeout: prazo; a janela da instância não apareceu; 0 tecla(s); 0 leitura(s) da API sem a conta");
        progress.window = true;
        progress.saw(Located::unasked(1, None, Reason::RequestNotConsumed));
        progress.presses = 3;
        let explained = progress.explain("timeout: prazo".into());
        assert!(explained.starts_with("timeout: prazo; último estado unknown (request_not_consumed)"), "{explained}");
        assert!(explained.contains("3 tecla(s)"), "{explained}");
        // Errors that already say what happened are left alone.
        assert_eq!(progress.explain("pad_unavailable: sem pad".into()), "pad_unavailable: sem pad");
    }
}
