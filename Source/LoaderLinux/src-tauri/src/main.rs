// The GUI is the product; a console window alongside it is not.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::path::PathBuf;

use ds2os_core::config::{self, LoaderSettings};
use ds2os_core::master::MasterClient;
use ds2os_core::steam::{GameDetection, GameType};
use ds2os_core::{launch, LaunchPlan, ServerEntry, Steam};

/// Errors reach the frontend as strings because the user reads them, so each
/// one has to say what failed and what to do about it.
type CommandResult<T> = Result<T, String>;

fn settings() -> LoaderSettings {
    LoaderSettings::load(&config::settings_path())
}

/// Servers the user imported by hand. A missing or corrupt file is simply an
/// empty list; it must never stop the master server list from loading.
fn manual_servers() -> Vec<ServerEntry> {
    std::fs::read_to_string(config::manual_servers_path())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

fn save_manual_servers(servers: &[ServerEntry]) -> CommandResult<()> {
    let path = config::manual_servers_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|error| format!("Não foi possível criar {}: {error}", parent.display()))?;
    }
    let body = serde_json::to_vec_pretty(servers).map_err(|error| error.to_string())?;
    std::fs::write(&path, body)
        .map_err(|error| format!("Não foi possível salvar em {}: {error}", path.display()))
}

/// Where Injector.dll and Injector.exe are expected to be.
fn injector_dir(settings: &LoaderSettings) -> PathBuf {
    settings.injector_dir.clone().unwrap_or_else(|| {
        std::env::current_exe()
            .ok()
            .and_then(|exe| exe.parent().map(PathBuf::from))
            .unwrap_or_else(|| PathBuf::from("."))
    })
}

#[tauri::command]
fn detect_game(game_type: GameType) -> CommandResult<GameDetection> {
    let steam = Steam::discover().map_err(|error| error.to_string())?;
    Ok(GameDetection::probe(&steam, game_type))
}

#[tauri::command]
async fn list_servers() -> CommandResult<Vec<ServerEntry>> {
    tauri::async_runtime::spawn_blocking(|| {
        let settings = settings();
        let mut servers = manual_servers();

        // A dead master server must not hide the servers the user typed in, so
        // its failure only matters when there is nothing else to show.
        match MasterClient::new(&settings.master_server_url).list_servers() {
            Ok(listed) => servers.extend(listed),
            Err(error) if servers.is_empty() => return Err(error.to_string()),
            Err(_) => {}
        }

        Ok(servers)
    })
    .await
    .map_err(|error| error.to_string())?
}

#[tauri::command]
fn import_server(mut server: ServerEntry) -> CommandResult<Vec<ServerEntry>> {
    if server.hostname.trim().is_empty() {
        return Err("Informe o endereço do servidor.".to_owned());
    }
    if !server.public_key.contains("BEGIN RSA PUBLIC KEY") {
        return Err("A chave pública precisa ser uma chave RSA.".to_owned());
    }

    server.manual_import = true;
    if server.id.trim().is_empty() {
        server.id = format!("{}:{}", server.hostname, server.port);
    }

    // Re-importing the same endpoint updates it rather than duplicating it.
    let mut servers = manual_servers();
    servers.retain(|existing| existing.id != server.id);
    servers.insert(0, server);

    save_manual_servers(&servers)?;
    Ok(servers)
}

#[tauri::command]
fn forget_server(server_id: String) -> CommandResult<Vec<ServerEntry>> {
    let mut servers = manual_servers();
    servers.retain(|existing| existing.id != server_id);
    save_manual_servers(&servers)?;
    Ok(servers)
}

#[tauri::command]
fn load_settings() -> CommandResult<LoaderSettings> {
    Ok(settings())
}

#[tauri::command]
fn save_settings(settings: LoaderSettings) -> CommandResult<()> {
    let path = config::settings_path();
    settings
        .save(&path)
        .map_err(|error| format!("Não foi possível salvar em {}: {error}", path.display()))
}

#[tauri::command]
async fn prepare_launch(server_id: String, password: Option<String>) -> CommandResult<LaunchPlan> {
    tauri::async_runtime::spawn_blocking(move || {
        let settings = settings();

        let mut server = list_all(&settings)?
            .into_iter()
            .find(|entry| entry.id == server_id)
            .ok_or_else(|| "Esse servidor não está mais na lista.".to_owned())?;

        // Passworded servers withhold the key until the password checks out.
        if server.public_key.trim().is_empty() {
            let password = password.unwrap_or_default();
            server.public_key = MasterClient::new(&settings.master_server_url)
                .public_key(&server.id, &password)
                .map_err(|error| error.to_string())?;
        }
        if server.public_key.trim().is_empty() {
            return Err("O servidor não forneceu uma chave pública.".to_owned());
        }

        let game_type = if server.is(GameType::DarkSouls3) {
            GameType::DarkSouls3
        } else {
            GameType::DarkSouls2
        };

        let steam = Steam::discover().map_err(|error| error.to_string())?;
        let install = steam
            .find_game(game_type)
            .ok_or_else(|| format!("{game_type:?} não está instalado na Steam."))?;

        let config = settings.to_injector_config(
            game_type,
            &server.name,
            &server.hostname,
            server.port,
            &server.public_key,
        );

        launch::prepare(&install, &config, &injector_dir(&settings))
            .map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| error.to_string())?
}

fn list_all(settings: &LoaderSettings) -> Result<Vec<ServerEntry>, String> {
    let mut servers = manual_servers();
    if let Ok(listed) = MasterClient::new(&settings.master_server_url).list_servers() {
        servers.extend(listed);
    }
    Ok(servers)
}

fn main() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            detect_game,
            list_servers,
            import_server,
            forget_server,
            load_settings,
            save_settings,
            prepare_launch
        ])
        .run(tauri::generate_context!())
        .expect("failed to start the ds2os loader");
}
