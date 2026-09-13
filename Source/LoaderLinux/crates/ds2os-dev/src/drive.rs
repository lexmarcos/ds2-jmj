//! Menu actions with per-instance identity, fresh state and a single deadline.
use std::time::{Duration, Instant};
use crate::{api, control::Deadline, env::Environment, game, observe, pad, probe::{self, Where}, screen::{self, GameWindow}};

pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(180);
#[derive(Debug, Clone, serde::Serialize)]
pub struct Arrival { pub character: String, pub seconds: f64 }

/// An unresolved account is never substituted with another window.
pub fn window_for(environment: &Environment, account: u8) -> Result<GameWindow, String> {
    let prefix = game::compat_data(environment, account)?;
    let pids = game::instance_pids(&prefix);
    screen::windows()?.into_iter().find(|w| w.pid.is_some_and(|p| pids.contains(&p)))
        .ok_or_else(|| format!("instance_unresolved: conta {account} sem janela pertencente ao seu prefixo"))
}

fn wait_for_window(env: &Environment, account: u8, deadline: Deadline) -> Result<GameWindow, String> {
    loop {
        deadline.remaining()?;
        if let Ok(w) = window_for(env, account) { return Ok(w); }
        deadline.sleep(Duration::from_millis(300))?;
    }
}

pub fn wait_title(env: &Environment, account: u8, timeout: Duration) -> Result<(), String> {
    let deadline = Deadline::after(timeout);
    wait_for_window(env, account, deadline)?;
    loop {
        deadline.remaining()?;
        match locate_until(env, account, deadline) {
            Where::Title => return Ok(()),
            Where::World => return Err("already_in_world: --no-enter exige a instância no título".into()),
            _ => deadline.sleep(Duration::from_millis(300))?,
        }
    }
}

pub fn locate(env: &Environment, account: u8) -> Where {
    locate_until(env, account, Deadline::after(Duration::from_secs(4)))
}
fn locate_until(env: &Environment, account: u8, deadline: Deadline) -> Where {
    if observe::processes(env, account).is_empty() { return Where::Unknown; }
    match (env.installs.iter().find(|i| i.account == account), deadline.remaining()) {
        (Some(i), Ok(left)) => probe::locate(&i.game_dir, left.min(Duration::from_secs(2))),
        _ => Where::Unknown,
    }
}

fn press(env: &Environment, account: u8, command: &str, deadline: Deadline) -> Result<(), String> {
    deadline.remaining()?;
    let window = window_for(env, account)?;
    screen::focus(&window)?;
    pad::send_until(1, command, deadline.remaining()?).map(|_| ())
}

pub fn enter(env: &Environment, instance: u8, expect: Option<&str>, timeout: Duration) -> Result<Arrival, String> {
    let deadline = Deadline::after(timeout);
    let started = Instant::now();
    let id = observe::steam_id(instance)?;
    let server = env.server.as_ref().ok_or("api_unavailable: servidor ausente")?;
    // Check identity/API before sending any input.
    api::players_until(server, api::web_port(&server.config), deadline.remaining()?)?;
    if !pad::running(1) { return Err("pad_unavailable: inicie o pad antes do jogo".into()); }
    wait_for_window(env, instance, deadline)?;
    let identity = observe::processes(env, instance);
    let mut presses = 0;
    let mut offline_since: Option<Instant> = None;
    let mut recoveries = 0;
    loop {
        deadline.remaining()?;
        if observe::processes(env, instance) != identity { return Err("process_changed: a instância reiniciou durante enter".into()); }
        match locate_until(env, instance, deadline) {
            Where::World => {
                let list = api::players_until(server, api::web_port(&server.config), deadline.remaining()?)?;
                if let Some(player) = observe::player_for(&list, &id)? {
                    if player.name.is_empty() { deadline.sleep(Duration::from_millis(300))?; continue; }
                    if let Some(expected) = expect {
                        if !player.name.eq_ignore_ascii_case(expected) {
                            return Err(format!("wrong_character: carregou {} e não {expected}", player.name));
                        }
                    }
                    return Ok(Arrival { character: player.name.clone(), seconds: started.elapsed().as_secs_f64() });
                }
                let since = offline_since.get_or_insert_with(Instant::now);
                if since.elapsed() > Duration::from_secs(45) {
                    if recoveries >= 2 { return Err("offline: entrou sem a conexão Steam esperada".into()); }
                    recoveries += 1;
                    leave_until(env, instance, deadline, false)?;
                    offline_since = None;
                }
            }
            Where::Title => {
                offline_since = None;
                press(env, instance, if presses % 2 == 0 { "press start" } else { "press a" }, deadline)?;
                presses += 1;
                deadline.sleep(Duration::from_millis(1200))?;
            }
            // Loading is not world; unknown never authorizes blind presses.
            Where::Loading | Where::Unknown => {}
        }
        deadline.sleep(Duration::from_millis(300))?;
    }
}

pub fn leave(env: &Environment, instance: u8, timeout: Duration) -> Result<f64, String> {
    let deadline = Deadline::after(timeout);
    match locate_until(env, instance, deadline) {
        Where::Title => return Ok(0.0),
        Where::World => {},
        _ => return Err("state_unknown: só é possível sair a partir de um mundo confirmado".into()),
    }
    leave_until(env, instance, deadline, false)
}

pub fn leave_now(env: &Environment, instance: u8, timeout: Duration, dismiss_first: bool) -> Result<f64, String> {
    leave_until(env, instance, Deadline::after(timeout), dismiss_first)
}

fn leave_until(env: &Environment, instance: u8, deadline: Deadline, dismiss_first: bool) -> Result<f64, String> {
    let started = Instant::now();
    let identity = observe::processes(env, instance);
    let walk = [("press start", 900), ("press rb", 250), ("press rb", 250), ("press rb", 250),
        ("press rb", 250), ("press rb", 400), ("dpad down", 250), ("dpad down", 350),
        ("press a", 700), ("dpad left", 350), ("press a", 0)];
    for attempt in 0..2 {
        if attempt > 0 || dismiss_first { press(env, instance, "press a", deadline)?; deadline.sleep(Duration::from_millis(900))?; }
        for (command, gap) in walk {
            if observe::processes(env, instance) != identity { return Err("process_changed: instância reiniciou durante leave".into()); }
            press(env, instance, command, deadline)?;
            if gap > 0 { deadline.sleep(Duration::from_millis(gap))?; }
        }
        let until = Instant::now() + Duration::from_secs(20);
        while Instant::now() < until {
            deadline.remaining()?;
            if locate_until(env, instance, deadline) == Where::Title { return Ok(started.elapsed().as_secs_f64()); }
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
