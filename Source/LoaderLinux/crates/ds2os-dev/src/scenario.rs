//! Small declarative runner. Assertions consume observations, never screenshots or absence of errors.
use std::time::{Duration, Instant};
use serde::Deserialize;
use serde_json::{json, Value};
use crate::{control::Deadline, env::Environment, observe, output, save};

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Scenario {
    schema_version: u8,
    name: String,
    instances: Vec<u8>,
    #[serde(default = "default_timeout")]
    timeout_seconds: u64,
    /// Games must already be stopped. Preserve originals, restore baseline,
    /// execute steps, stop owned clients and restore originals even on failure.
    baseline: Option<String>,
    /// `reset` puts the request-driven hooks of every running declared instance
    /// back to a fresh arrival's state before the steps, and again after them
    /// when there is no baseline to stop the games. `keep` (the default) leaves
    /// whatever the previous test set.
    #[serde(default)]
    hook_state: HookState,
    steps: Vec<Step>,
}
fn default_timeout() -> u64 { 300 }

#[derive(Debug, Default, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
enum HookState { Reset, #[default] Keep }

/// Resets the hooks of each running declared instance; stopped ones start fresh anyway.
fn reset_hooks(env: &Environment, accounts: &[u8], phase: &str) -> Result<(), String> {
    for &account in accounts {
        if observe::processes(env, account).is_empty() { continue; }
        let install = crate::install_for(env, account)?;
        let results = crate::hooks::reset(install)?;
        output::event("scenario_hooks_reset", json!({"phase": phase, "instance": account, "results": results}));
        if let Some(failed) = results.iter().find(|r| r.outcome == "failed") {
            return Err(format!("hooks_not_reset: conta {account} {}: {}", failed.hook, failed.error.clone().unwrap_or_default()));
        }
    }
    Ok(())
}

#[derive(Debug, Deserialize)]
#[serde(tag = "action", rename_all = "snake_case", deny_unknown_fields)]
enum Step {
    Launch { instance: u8 },
    Enter { instance: u8, character: Option<String> },
    Leave { instance: u8 },
    Input { instance: u8, command: String },
    Goto { instance: u8, x: f32, z: f32, #[serde(default = "default_radius")] radius: f32 },
    Assert { instance: u8, pointer: String, equals: Value },
    Wait { instance: u8, pointer: String, equals: Value, seconds: u64 },
    Observe,
    Screenshot,
    /// Kills the local character; the death hook must not be in observe mode.
    Kill { instance: u8 },
    /// Teleports the local character within its loaded map (see `teleport`).
    Teleport { instance: u8, x: f32, y: f32, z: f32 },
    /// Moves the local character onto another map (see `goto-map`).
    GotoMap { instance: u8, map: String, x: f32, y: f32, z: f32 },
}
fn default_radius() -> f32 { 2.0 }
impl Step {
    fn instance(&self) -> Option<u8> {
        match self {
            Self::Launch { instance } | Self::Enter { instance, .. } | Self::Leave { instance } |
            Self::Input { instance, .. } | Self::Goto { instance, .. } | Self::Assert { instance, .. } |
            Self::Wait { instance, .. } | Self::Kill { instance } | Self::Teleport { instance, .. } |
            Self::GotoMap { instance, .. } => Some(*instance),
            _ => None,
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
enum Verdict { Passed, Failed, Inconclusive }

fn evaluate(observation: &Value, instance: u8, pointer: &str, expected: &Value) -> Verdict {
    let item = observation.get("instances").and_then(Value::as_array)
        .and_then(|items| items.iter().find(|i| i["instance"] == instance));
    let Some(item) = item else { return Verdict::Inconclusive; };
    // A character read outside a confirmed world proves nothing.
    if pointer.starts_with("/character/") && item["state"] != "world" { return Verdict::Inconclusive; }
    // Do not let a stale API record prove character, area or connection.
    if (pointer.starts_with("/player/") || pointer == "/serverConnected")
        && (item["state"] != "world" || observation.get("serverError").is_some_and(|v| !v.is_null())) {
        return Verdict::Inconclusive;
    }
    match item.pointer(pointer) {
        None | Some(Value::Null) => Verdict::Inconclusive,
        Some(v) if (pointer == "/state" || pointer == "/session/role") && v == "unknown" => Verdict::Inconclusive,
        Some(v) if v == expected => Verdict::Passed,
        Some(_) => Verdict::Failed,
    }
}

fn validate(scenario: &Scenario) -> Result<(), String> {
    if scenario.schema_version != 1 || scenario.name.is_empty() || scenario.steps.is_empty() || scenario.steps.len() > 200 {
        return Err("invalid_scenario: schemaVersion 1, nome e 1..200 passos obrigatórios".into());
    }
    if !matches!(scenario.instances.as_slice(), [1] | [2] | [1, 2]) || !(1..=3600).contains(&scenario.timeout_seconds) {
        return Err("invalid_scenario: instances deve ser [1], [2] ou [1,2]; timeoutSeconds entre 1 e 3600".into());
    }
    if let Some(label) = &scenario.baseline { save::validate_label(label)?; }
    let mut assertions = 0;
    for step in &scenario.steps {
        if step.instance().is_some_and(|i| !scenario.instances.contains(&i)) { return Err("passo usa instância não declarada".into()); }
        match step {
            Step::Assert { pointer, equals, .. } | Step::Wait { pointer, equals, .. } => {
                assertions += 1;
                if equals.is_null() || equals == "unknown" || !(matches!(pointer.as_str(), "/state" | "/serverConnected" | "/player/name" |
                    "/player/location" | "/pose/archetype" | "/p2pSessionVerified" | "/character/hp" | "/character/hpMax" |
                    "/character/souls" | "/character/deaths" | "/character/hollow" | "/character/hollowState" | "/character/role" |
                    "/character/bonfire/id" | "/character/bonfire/map" | "/session/role") || pointer.starts_with("/hooks/hooks/")) {
                    return Err(format!("invalid_assertion: {pointer}; valor desconhecido não pode aprovar teste"));
                }
            }
            Step::Input { command, .. } => crate::pad::validate(command)?,
            Step::Goto { x, z, radius, .. } if !x.is_finite() || !z.is_finite() || !radius.is_finite() || *radius <= 0.0 => {
                return Err("invalid_target: coordenadas finitas e raio positivo".into());
            }
            Step::Teleport { x, y, z, .. } | Step::GotoMap { x, y, z, .. } if ![x, y, z].iter().all(|v| v.is_finite()) => {
                return Err("invalid_target: coordenadas finitas".into());
            }
            Step::GotoMap { map, .. } => { crate::backread::parse_map(map)?; }
            _ => {},
        }
        if matches!(step, Step::Input { command, .. } if ["quit", "ping", "neutral"].contains(&command.as_str())) {
            return Err("use input apenas para botões, direcionais, gatilhos e analógicos".into());
        }
        if matches!(step, Step::Wait { seconds, .. } if *seconds == 0 || *seconds > scenario.timeout_seconds) {
            return Err("wait precisa de prazo positivo dentro do timeout do cenário".into());
        }
    }
    if assertions == 0 { return Err("invalid_scenario: pelo menos uma assertion é obrigatória".into()); }
    Ok(())
}

fn builtin() -> Value {
    json!({"schemaVersion": 1, "name": "world-ready", "instances": [1,2], "timeoutSeconds": 60,
        "steps": [
            {"action":"assert", "instance":1, "pointer":"/state", "equals":"world"},
            {"action":"assert", "instance":2, "pointer":"/state", "equals":"world"},
            {"action":"assert", "instance":1, "pointer":"/serverConnected", "equals":true},
            {"action":"assert", "instance":2, "pointer":"/serverConnected", "equals":true}
        ]})
}

pub fn validate_file(source: &str) -> Result<(), String> {
    let raw = if source == "world-ready" { serde_json::to_vec(&builtin()).unwrap() }
        else { std::fs::read(source).map_err(|e| format!("scenario_read: {e}"))? };
    let scenario: Scenario = serde_json::from_slice(&raw).map_err(|e| format!("invalid_scenario: {e}"))?;
    validate(&scenario)?;
    output::data(json!({"valid":true,"name":scenario.name,"steps":scenario.steps.len(),"instances":scenario.instances}));
    println!("cenário {} válido, {} passos", scenario.name, scenario.steps.len());
    Ok(())
}

pub fn run(env: &Environment, source: &str) -> Result<(), String> {
    let raw = if source == "world-ready" { serde_json::to_vec_pretty(&builtin()).unwrap() }
        else { std::fs::read(source).map_err(|e| format!("scenario_read: {e}"))? };
    let scenario: Scenario = serde_json::from_slice(&raw).map_err(|e| format!("invalid_scenario: {e}"))?;
    validate(&scenario)?;
    std::fs::write(output::dir().join("scenario.json"), &raw).map_err(|e| e.to_string())?;
    output::manifest(env)?;
    let selection = match scenario.instances.as_slice() { [1] => "1", [2] => "2", _ => "both" };
    let rescue = format!("run-{}", output::id());
    let deadline = Deadline::after(Duration::from_secs(scenario.timeout_seconds));
    let mut cleanup_needed = false;
    let mut completed = 0usize;
    let result = (|| {
        // No mutation before identities, installs and fixtures have all been checked.
        let mut ids = Vec::new();
        for &account in &scenario.instances {
            crate::install_for(env, account)?;
            let id = observe::steam_id(account)?;
            if ids.contains(&id) { return Err("duplicate_account: cenários exigem Steam IDs diferentes".into()); }
            ids.push(id);
        }
        if let Some(label) = &scenario.baseline {
            for &account in &scenario.instances {
                if !observe::processes(env, account).is_empty() { return Err("fixture_busy: feche as instâncias antes de usar baseline".into()); }
                if !save::snapshot_path(account, label).is_file() { return Err(format!("fixture_missing: conta {account}, {label}")); }
            }
            save::backup(env, selection, Some(&rescue))?;
            cleanup_needed = true;
            let snapshots: Vec<_> = scenario.instances.iter().map(|i| json!({"instance": i,
                "baseline": output::fingerprint(&save::snapshot_path(*i, label)),
                "original": output::fingerprint(&save::snapshot_path(*i, &rescue))})).collect();
            std::fs::write(output::dir().join("fixtures.json"), serde_json::to_vec_pretty(&snapshots).unwrap()).map_err(|e| e.to_string())?;
            save::restore(env, selection, label, None)?;
        }
        if scenario.hook_state == HookState::Reset { reset_hooks(env, &scenario.instances, "before")?; }
        for (index, step) in scenario.steps.iter().enumerate() {
            deadline.remaining()?;
            // Byte offsets of every log, so each step's own lines can be cut out afterwards.
            let cursors = output::log_cursors(env);
            output::event("step_started", json!({"index": index, "step": format!("{step:?}"), "logBytes": output::log_sizes(&cursors)}));
            let result = execute(env, step, &scenario.instances, deadline);
            output::event("step_finished", json!({"index": index, "ok": result.is_ok(), "error": result.as_ref().err(), "logBytes": output::log_sizes(&cursors)}));
            // Evidence failure is recorded separately from the assertion verdict.
            capture(env, &format!("step-{index}"));
            result?;
            completed += 1;
        }
        if scenario.hook_state == HookState::Reset && scenario.baseline.is_none() { reset_hooks(env, &scenario.instances, "after")?; }
        Ok(())
    })();
    if result.is_err() { capture(env, "failure"); }
    // Cleanup has its own bounded process-stop operations and runs after cancellation too.
    let cleanup = if cleanup_needed {
        output::event("cleanup_started", json!({"restore": rescue}));
        let _ = crate::pad::send_until(1, "neutral", Duration::from_secs(6));
        let result = save::restore(env, selection, &rescue, Some(crate::game::StopGuard::Cleanup));
        output::event("cleanup_finished", json!({"ok": result.is_ok(), "error": result.as_ref().err()}));
        result
    } else { Ok(()) };
    output::data(json!({"scenario": scenario.name, "completedSteps": completed, "totalSteps": scenario.steps.len(),
        "cleanupRequired": cleanup_needed, "cleanupOk": cleanup.is_ok(), "originalSnapshot": if cleanup_needed { Some(rescue) } else { None },
        "executionError": result.as_ref().err(), "cleanupError": cleanup.as_ref().err()}));
    if let Err(e) = cleanup { output::outcome("failed"); return Err(format!("cleanup_failed: {e}; originais preservados no snapshot")); }
    if result.as_ref().is_err_and(|e| e.starts_with("inconclusive:") || e.starts_with("identity_unresolved:") || e.starts_with("api_unavailable:")) {
        output::outcome("inconclusive");
    }
    result
}

fn capture(env: &Environment, name: &str) {
    let dir = output::dir().join(name);
    if let Err(e) = crate::shot_into(env, Some(dir)) { output::event("capture_error", json!(e)); }
}

fn execute(env: &Environment, step: &Step, accounts: &[u8], deadline: Deadline) -> Result<(), String> {
    match step {
        Step::Launch { instance } => {
            if !crate::pad::running(1) { return Err("pad_unavailable: inicie o pad antes de launch".into()); }
            let home = crate::settings::HarnessConfig::load().second_steam_home;
            crate::game::launch(env, *instance, home.as_deref()).map(|_| ())
        }
        Step::Enter { instance, character } => {
            let expected = character.clone().or_else(|| crate::drive::expected_character(&crate::settings::HarnessConfig::load(), *instance));
            crate::drive::enter(env, *instance, expected.as_deref(), deadline.remaining()?).map(|_| ())
        }
        Step::Leave { instance } => crate::drive::leave(env, *instance, deadline.remaining()?).map(|_| ()),
        Step::Input { instance, command } => {
            crate::screen::focus(&crate::drive::window_for(env, *instance)?)?;
            crate::pad::send_until(1, command, deadline.remaining()?).map(|_| ())
        }
        Step::Goto { instance, x, z, radius } => {
            let install = crate::install_for(env, *instance)?;
            let result = crate::nav::walk_to(env, &install.game_dir, *instance, (*x, *z), crate::nav::Plan {
                radius: *radius, timeout: deadline.remaining()?, ..Default::default()
            }, |n, pose, distance| output::event("navigation", json!({"step":n, "pose":pose, "distance":distance})))?;
            match result { crate::nav::Outcome::Arrived { .. } => Ok(()),
                crate::nav::Outcome::LostPlayer { .. } => Err(format!("inconclusive: {result:?}")),
                _ => Err(format!("navigation_failed: {result:?}")) }
        }
        Step::Assert { instance, pointer, equals } => assertion(env, *instance, pointer, equals, None, deadline),
        Step::Wait { instance, pointer, equals, seconds } => assertion(env, *instance, pointer, equals, Some(Duration::from_secs(*seconds)), deadline),
        Step::Observe => { let value = observe::collect_until(env, accounts, deadline); output::event("observation", json!(value)); Ok(()) },
        Step::Screenshot => crate::shot_into(env, Some(output::dir().join("screenshots"))),
        Step::Kill { instance } => crate::death::kill(env, *instance, false, deadline.remaining()?.min(Duration::from_secs(15)))
            .map(|data| output::event("kill", data)),
        Step::Teleport { instance, x, y, z } => crate::teleport::teleport(env, *instance, [*x, *y, *z])
            .map(|data| output::event("teleport", data)),
        Step::GotoMap { instance, map, x, y, z } => crate::backread::goto_map(env, *instance, map, [*x, *y, *z])
            .map(|data| output::event("goto_map", data)),
    }
}

fn assertion(env: &Environment, instance: u8, pointer: &str, equals: &Value, wait: Option<Duration>, deadline: Deadline) -> Result<(), String> {
    let window = wait.unwrap_or(deadline.remaining()?).min(deadline.remaining()?);
    let until = Instant::now() + window;
    let observation_deadline = Deadline::after(window);
    loop {
        deadline.remaining()?;
        let include = observe::Include { character: pointer.starts_with("/character/"),
            session: pointer.starts_with("/session/") || pointer == "/p2pSessionVerified" };
        let observation = json!(observe::collect_with(env, &[instance], observation_deadline, include));
        let verdict = evaluate(&observation, instance, pointer, equals);
        output::event("assertion", json!({"instance": instance, "pointer": pointer, "expected": equals,
            "verdict": format!("{verdict:?}").to_lowercase(), "observation": observation}));
        if verdict == Verdict::Passed { observation_deadline.remaining()?; return Ok(()); }
        if wait.is_none() || Instant::now() >= until {
            return Err(format!("{}: conta {instance}, {pointer} esperado {equals}",
                if verdict == Verdict::Inconclusive { "inconclusive" } else { "assertion_failed" }));
        }
        deadline.sleep(Duration::from_millis(200))?;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unknown_and_missing_evidence_cannot_pass() {
        let value = json!({"instances":[{"instance":2,"state":"unknown", "p2pSessionVerified":null}]});
        assert_eq!(evaluate(&value, 2, "/state", &json!("world")), Verdict::Inconclusive);
        assert_eq!(evaluate(&value, 2, "/p2pSessionVerified", &json!(true)), Verdict::Inconclusive);
        assert_eq!(evaluate(&value, 1, "/state", &json!("world")), Verdict::Inconclusive);
    }
    #[test]
    fn known_mismatch_is_failure_and_other_instance_cannot_satisfy_assertion() {
        let value = json!({"instances":[{"instance":1,"state":"world"},{"instance":2,"state":"title"}]});
        assert_eq!(evaluate(&value, 1, "/state", &json!("world")), Verdict::Passed);
        assert_eq!(evaluate(&value, 2, "/state", &json!("world")), Verdict::Failed);
    }
    #[test]
    fn a_character_outside_a_confirmed_world_is_not_evidence() {
        let value = json!({"instances":[{"instance":1,"state":"loading", "character":{"hp":0}}, {"instance":2,"state":"world", "character":{"hp":0}}]});
        assert_eq!(evaluate(&value, 1, "/character/hp", &json!(0)), Verdict::Inconclusive);
        assert_eq!(evaluate(&value, 2, "/character/hp", &json!(0)), Verdict::Passed);
        assert_eq!(evaluate(&value, 2, "/character/hollow", &json!(0)), Verdict::Inconclusive);
        let mut scenario = builtin();
        scenario["steps"] = json!([{"action":"assert","instance":1,"pointer":"/character/hollow","equals":0}]);
        assert!(validate(&serde_json::from_value(scenario.clone()).unwrap()).is_ok());
        scenario["steps"] = json!([{"action":"assert","instance":1,"pointer":"/character/address","equals":"0x1"}]);
        assert!(validate(&serde_json::from_value(scenario).unwrap()).is_err());
    }

    #[test]
    fn stale_server_presence_is_not_connection_evidence() {
        let value = json!({"serverError":"timeout", "instances":[{"instance":1,"state":"world", "serverConnected":true}]});
        assert_eq!(evaluate(&value, 1, "/serverConnected", &json!(true)), Verdict::Inconclusive);
    }
    #[test]
    fn scenarios_without_assertions_or_with_unknown_expected_are_rejected() {
        let mut value = builtin();
        let good: Scenario = serde_json::from_value(value.clone()).unwrap();
        assert!(validate(&good).is_ok());
        value["steps"] = json!([{"action":"observe"}]);
        assert!(validate(&serde_json::from_value(value.clone()).unwrap()).is_err());
        value["steps"] = json!([{"action":"assert","instance":1,"pointer":"/state","equals":"unknown"}]);
        assert!(validate(&serde_json::from_value(value).unwrap()).is_err());
    }

    #[test]
    fn hook_state_defaults_to_keep_and_rejects_anything_else() {
        let mut value = builtin();
        let kept: Scenario = serde_json::from_value(value.clone()).unwrap();
        assert_eq!(kept.hook_state, HookState::Keep);
        value["hookState"] = json!("reset");
        assert_eq!(serde_json::from_value::<Scenario>(value.clone()).unwrap().hook_state, HookState::Reset);
        value["hookState"] = json!("clean");
        assert!(serde_json::from_value::<Scenario>(value).is_err());
    }
}
