//! Client for the DS3OS master server.
//!
//! The contract is set by the existing Windows loader (`Source/Loader/Api/
//! MasterServerApi.cs`) and the servers already deployed against it, so nothing
//! here is ours to redesign:
//!
//! - `GET  {base}/api/v1/servers`
//! - `POST {base}/api/v1/servers/{id}/public_key`  body `{"Password": "..."}`
//!
//! Every response carries a `Status` that must read `success`, whatever the
//! HTTP status code was.

use std::time::Duration;

use serde::{Deserialize, Serialize};

use crate::steam::GameType;

/// A GUI must never hang on a dead master server.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const RESPONSE_TIMEOUT: Duration = Duration::from_secs(15);

#[derive(Debug, thiserror::Error)]
pub enum MasterError {
    #[error("could not reach the master server at {url}: {source}")]
    Transport { url: String, source: Box<ureq::Error> },
    #[error("the master server sent a response this loader could not read: {0}")]
    Malformed(String),
    #[error("the master server refused the request: {0}")]
    Refused(String),
}

/// One server as the master server describes it.
///
/// The frontend reads a subset of these fields; the rest are what a launch
/// needs. Field names mirror `ServerEntry` in `src/lib/api.ts`.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ServerEntry {
    #[serde(default, alias = "Id")]
    pub id: String,
    #[serde(default, alias = "Name")]
    pub name: String,
    #[serde(default, alias = "Description")]
    pub description: String,
    #[serde(default, alias = "Hostname")]
    pub hostname: String,
    #[serde(default, alias = "PrivateHostname")]
    pub private_hostname: String,
    #[serde(default, alias = "IpAddress")]
    pub ip_address: String,
    #[serde(default, alias = "Port")]
    pub port: i32,
    #[serde(default, alias = "PublicKey")]
    pub public_key: String,
    #[serde(default, alias = "PlayerCount")]
    pub player_count: u32,
    #[serde(default, alias = "PasswordRequired")]
    pub password_required: bool,
    #[serde(default, alias = "GameType")]
    pub game_type: String,
    /// True for servers the user typed in rather than ones the master listed.
    #[serde(default, alias = "ManualImport")]
    pub manual_import: bool,
}

impl ServerEntry {
    /// Whether this server runs the given game.
    pub fn is(&self, game_type: GameType) -> bool {
        self.game_type.eq_ignore_ascii_case(&format!("{game_type:?}"))
    }
}

#[derive(Debug, Deserialize)]
struct ListServersResponse {
    #[serde(default, alias = "status")]
    #[serde(rename = "Status")]
    status: String,
    #[serde(default, alias = "message")]
    #[serde(rename = "Message")]
    message: String,
    #[serde(default, alias = "servers")]
    #[serde(rename = "Servers")]
    servers: Vec<ServerEntry>,
}

#[derive(Debug, Deserialize)]
struct PublicKeyResponse {
    #[serde(default, alias = "status")]
    #[serde(rename = "Status")]
    status: String,
    #[serde(default, alias = "message")]
    #[serde(rename = "Message")]
    message: String,
    #[serde(default, alias = "publicKey", alias = "public_key")]
    #[serde(rename = "PublicKey")]
    public_key: String,
}

#[derive(Debug, Serialize)]
struct PublicKeyRequest<'a> {
    #[serde(rename = "Password")]
    password: &'a str,
}

pub struct MasterClient {
    base_url: String,
    agent: ureq::Agent,
}

impl MasterClient {
    pub fn new(base_url: impl Into<String>) -> Self {
        let config = ureq::Agent::config_builder()
            .timeout_connect(Some(CONNECT_TIMEOUT))
            .timeout_global(Some(RESPONSE_TIMEOUT))
            .build();

        Self {
            base_url: base_url.into().trim_end_matches('/').to_owned(),
            agent: config.into(),
        }
    }

    /// Lists every server the master knows about.
    ///
    /// Servers with no id are keyed by address, matching what the Windows
    /// loader does, so the frontend always has a stable selection key.
    pub fn list_servers(&self) -> Result<Vec<ServerEntry>, MasterError> {
        let url = format!("{}/api/v1/servers", self.base_url);

        let mut response = self
            .agent
            .get(&url)
            .header("Accept", "application/json")
            .call()
            .map_err(|source| MasterError::Transport { url: url.clone(), source: Box::new(source) })?;

        let payload: ListServersResponse = response
            .body_mut()
            .read_json()
            .map_err(|error| MasterError::Malformed(error.to_string()))?;

        check_status(&payload.status, &payload.message)?;

        Ok(payload
            .servers
            .into_iter()
            .map(|mut server| {
                if server.id.is_empty() {
                    server.id = server.ip_address.clone();
                }
                server
            })
            .collect())
    }

    /// Fetches a server's public key, which passworded servers withhold until
    /// the password is right.
    pub fn public_key(&self, server_id: &str, password: &str) -> Result<String, MasterError> {
        let url = format!("{}/api/v1/servers/{server_id}/public_key", self.base_url);

        let mut response = self
            .agent
            .post(&url)
            .header("Accept", "application/json")
            .send_json(PublicKeyRequest { password })
            .map_err(|source| MasterError::Transport { url: url.clone(), source: Box::new(source) })?;

        let payload: PublicKeyResponse = response
            .body_mut()
            .read_json()
            .map_err(|error| MasterError::Malformed(error.to_string()))?;

        check_status(&payload.status, &payload.message)?;

        Ok(payload.public_key)
    }
}

fn check_status(status: &str, message: &str) -> Result<(), MasterError> {
    if status.eq_ignore_ascii_case("success") {
        return Ok(());
    }
    Err(MasterError::Refused(if message.is_empty() {
        format!("status {status:?}")
    } else {
        message.to_owned()
    }))
}
