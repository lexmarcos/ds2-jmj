// The GUI is the product; a console window alongside it is not.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use ds2os_core::config::{self, LoaderSettings};
use ds2os_core::steam::{GameDetection, GameType};
use ds2os_core::{LaunchPlan, Steam};
use serde::{Deserialize, Serialize};

/// One entry in the server list.
///
/// This mirrors `ServerEntry` in `src/lib/api.ts`; change the two together.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct ServerEntry {
    id: String,
    name: String,
    description: String,
    hostname: String,
    port: i32,
    player_count: u32,
    game_type: GameType,
    password_required: bool,
    manual_import: bool,
}

/// Errors reach the frontend as strings because the user reads them. Each one
/// has to say what failed and what to do next.
type CommandResult<T> = Result<T, String>;

#[tauri::command]
fn detect_game(game_type: GameType) -> CommandResult<GameDetection> {
    let steam = Steam::discover().map_err(|error| error.to_string())?;
    Ok(GameDetection::probe(&steam, game_type))
}

#[tauri::command]
fn list_servers() -> CommandResult<Vec<ServerEntry>> {
    // The master server client is the next piece of work. Returning an error
    // rather than an empty list keeps the UI honest about why it is empty.
    Err("A lista de servidores ainda não está conectada ao master server.".to_owned())
}

#[tauri::command]
fn load_settings() -> CommandResult<LoaderSettings> {
    Ok(LoaderSettings::load(&config::settings_path()))
}

#[tauri::command]
fn save_settings(settings: LoaderSettings) -> CommandResult<()> {
    let path = config::settings_path();
    settings
        .save(&path)
        .map_err(|error| format!("Não foi possível salvar em {}: {error}", path.display()))
}

#[tauri::command]
fn prepare_launch(server_id: String) -> CommandResult<LaunchPlan> {
    let _ = server_id;
    // Needs the server list first, plus a Windows-built Injector.exe next to
    // Injector.dll. Both are tracked as follow-up work.
    Err("O lançamento ainda depende do Injector.exe e da lista de servidores.".to_owned())
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            detect_game,
            list_servers,
            load_settings,
            save_settings,
            prepare_launch
        ])
        .run(tauri::generate_context!())
        .expect("failed to start the ds2os loader");
}
