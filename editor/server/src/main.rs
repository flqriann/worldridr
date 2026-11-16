//! Forge Server – publishing, game catalog, multiplayer sessions and DataStores.
//!
//! ```text
//! forge-server [--port 8787] [--host 0.0.0.0] [--data ./server-data] [--publish-key KEY]
//! ```
//!
//! HTTP API (JSON):
//! - `GET  /api/health`
//! - `GET  /api/games`                                   catalog (+ players online)
//! - `POST /api/games`                     (key)         create a game
//! - `GET  /api/games/:id`                               game + version history + sessions
//! - `PATCH /api/games/:id`                (key)         rename / describe
//! - `POST /api/games/:id/versions`        (key)         publish (git-like, rejects stale parents)
//! - `POST /api/games/:id/live`            (key)         set live version (rollback)
//! - `GET  /api/games/:id/live`                          live project file
//! - `GET  /api/games/:id/versions/:n`                   project file of a version
//! - `GET|PUT|DELETE /api/games/:id/datastore/:store/:key` (PUT/DELETE: host session token)
//! - `GET  /ws/:id?name=..&version=..`                   WebSocket game session
//!
//! "(key)" endpoints require the header `x-forge-key: <publish key>`.

mod realtime;
mod store;

use axum::extract::{DefaultBodyLimit, Path, Query, State, WebSocketUpgrade};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use realtime::Sessions;
use serde::Deserialize;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::Arc;
use store::{Store, StoreError};
use tower_http::cors::CorsLayer;

struct AppState {
    store: Store,
    sessions: Arc<Sessions>,
    publish_key: String,
}
type Shared = Arc<AppState>;

struct ApiError(StatusCode, String);
impl IntoResponse for ApiError {
    fn into_response(self) -> Response { (self.0, Json(json!({ "error": self.1 }))).into_response() }
}
impl From<StoreError> for ApiError {
    fn from(e: StoreError) -> Self {
        match e {
            StoreError::NotFound(m) => ApiError(StatusCode::NOT_FOUND, format!("not found: {m}")),
            StoreError::Conflict { latest } => ApiError(StatusCode::CONFLICT, format!("version conflict: the latest published version is {}. Update your parent version (or force publish).", latest.map(|n| n.to_string()).unwrap_or_else(|| "none".into()))),
            StoreError::Invalid(m) => ApiError(StatusCode::BAD_REQUEST, m),
            StoreError::Io(m) => { eprintln!("[io] {m}"); ApiError(StatusCode::INTERNAL_SERVER_ERROR, "storage error".into()) }
        }
    }
}
type ApiResult<T> = Result<T, ApiError>;

fn require_key(state: &AppState, headers: &HeaderMap) -> ApiResult<()> {
    let given = headers.get("x-forge-key").and_then(|v| v.to_str().ok()).unwrap_or("");
    // constant-time-ish comparison
    let ok = given.len() == state.publish_key.len() && given.bytes().zip(state.publish_key.bytes()).fold(0u8, |a, (x, y)| a | (x ^ y)) == 0;
    if ok { Ok(()) } else { Err(ApiError(StatusCode::UNAUTHORIZED, "missing or wrong publish key (x-forge-key)".into())) }
}

async fn health(State(s): State<Shared>) -> Json<Value> {
    Json(json!({ "name": "forge-server", "version": env!("CARGO_PKG_VERSION"), "games": s.store.list().await.len() }))
}

async fn list_games(State(s): State<Shared>) -> Json<Value> {
    let counts = s.sessions.player_counts().await;
    let games: Vec<Value> = s.store.list().await.into_iter().map(|(g, n)| {
        let mut v = serde_json::to_value(&g).unwrap();
        v["versions"] = json!(n);
        v["playersOnline"] = json!(counts.get(&g.id).copied().unwrap_or(0));
        v
    }).collect();
    Json(json!({ "games": games }))
}

#[derive(Deserialize)]
struct CreateGame { name: String, #[serde(default)] description: String, #[serde(default)] author: String }

async fn create_game(State(s): State<Shared>, headers: HeaderMap, Json(body): Json<CreateGame>) -> ApiResult<Json<Value>> {
    require_key(&s, &headers)?;
    let g = s.store.create_game(&body.name, &body.description, &body.author).await?;
    println!("[api] created game {} ({})", g.id, g.name);
    Ok(Json(serde_json::to_value(g).unwrap()))
}

async fn get_game(State(s): State<Shared>, Path(id): Path<String>) -> ApiResult<Json<Value>> {
    let (g, versions) = s.store.get(&id).await.ok_or(StoreError::NotFound(format!("game {id}")))?;
    let mut v = serde_json::to_value(&g).unwrap();
    v["history"] = serde_json::to_value(&versions).unwrap();
    v["sessions"] = json!(s.sessions.sessions_json(&id).await);
    Ok(Json(v))
}

#[derive(Deserialize)]
struct PatchGame { name: Option<String>, description: Option<String> }

async fn patch_game(State(s): State<Shared>, headers: HeaderMap, Path(id): Path<String>, Json(body): Json<PatchGame>) -> ApiResult<Json<Value>> {
    require_key(&s, &headers)?;
    Ok(Json(serde_json::to_value(s.store.update_meta(&id, body.name, body.description).await?).unwrap()))
}

#[derive(Deserialize)]
struct Publish {
    parent: Option<u32>,
    #[serde(default)] message: String,
    #[serde(default)] author: String,
    project: String,
    thumbnail: Option<String>,
    #[serde(default = "yes")] live: bool,
    #[serde(default)] force: bool,
}
fn yes() -> bool { true }

async fn publish(State(s): State<Shared>, headers: HeaderMap, Path(id): Path<String>, Json(body): Json<Publish>) -> ApiResult<Json<Value>> {
    require_key(&s, &headers)?;
    let v = s.store.publish(&id, body.parent, &body.message, &body.author, body.project.as_bytes(), body.thumbnail, body.live, body.force).await?;
    println!("[api] published {} v{} ({} bytes): {}", id, v.number, v.size, v.message);
    Ok(Json(serde_json::to_value(v).unwrap()))
}

#[derive(Deserialize)]
struct SetLive { version: u32 }

async fn set_live(State(s): State<Shared>, headers: HeaderMap, Path(id): Path<String>, Json(body): Json<SetLive>) -> ApiResult<Json<Value>> {
    require_key(&s, &headers)?;
    let g = s.store.set_live(&id, body.version).await?;
    println!("[api] {} live -> v{}", id, body.version);
    Ok(Json(serde_json::to_value(g).unwrap()))
}

async fn project_response(s: &AppState, id: &str, n: Option<u32>) -> ApiResult<Response> {
    let (v, data) = s.store.content(id, n).await?;
    Ok(([("content-type", "application/json"), ("x-forge-version", &v.number.to_string())], data).into_response())
}
async fn live_project(State(s): State<Shared>, Path(id): Path<String>) -> ApiResult<Response> { project_response(&s, &id, None).await }
async fn version_project(State(s): State<Shared>, Path((id, n)): Path<(String, u32)>) -> ApiResult<Response> { project_response(&s, &id, Some(n)).await }

fn session_token(headers: &HeaderMap) -> String { headers.get("x-forge-session").and_then(|v| v.to_str().ok()).unwrap_or("").to_string() }

async fn ds_get(State(s): State<Shared>, Path((id, store, key)): Path<(String, String, String)>) -> ApiResult<Json<Value>> {
    Ok(Json(json!({ "value": s.store.ds_get(&id, &store, &key).await? })))
}

async fn ds_put(State(s): State<Shared>, headers: HeaderMap, Path((id, store, key)): Path<(String, String, String)>, Json(body): Json<Value>) -> ApiResult<Json<Value>> {
    // only the host of a running session (which runs the server scripts) or the publisher may write
    if !s.sessions.is_host_token(&id, &session_token(&headers)).await { require_key(&s, &headers)?; }
    let prev = s.store.ds_set(&id, &store, &key, Some(body.get("value").cloned().unwrap_or(Value::Null))).await?;
    Ok(Json(json!({ "previous": prev })))
}

async fn ds_delete(State(s): State<Shared>, headers: HeaderMap, Path((id, store, key)): Path<(String, String, String)>) -> ApiResult<Json<Value>> {
    if !s.sessions.is_host_token(&id, &session_token(&headers)).await { require_key(&s, &headers)?; }
    Ok(Json(json!({ "previous": s.store.ds_set(&id, &store, &key, None).await? })))
}

async fn ws(State(s): State<Shared>, Path(id): Path<String>, Query(q): Query<HashMap<String, String>>, upgrade: WebSocketUpgrade) -> ApiResult<Response> {
    let (game, _) = s.store.get(&id).await.ok_or(StoreError::NotFound(format!("game {id}")))?;
    let version = match q.get("version").and_then(|v| v.parse().ok()) { Some(v) => v, None => game.live.ok_or(StoreError::NotFound("game has no live version".into()))? };
    let name: String = q.get("name").map(|n| n.trim().chars().filter(|c| !c.is_control()).take(24).collect()).filter(|n: &String| !n.is_empty()).unwrap_or_else(|| "Player".into());
    let sessions = s.sessions.clone();
    Ok(upgrade.max_message_size(16 * 1024 * 1024).on_upgrade(move |socket| sessions.run(socket, id, version, name)))
}

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1).cloned())
}

#[tokio::main]
async fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.iter().any(|a| a == "--help" || a == "-h") {
        println!("forge-server [--port 8787] [--host 0.0.0.0] [--data ./server-data] [--publish-key KEY]");
        return;
    }
    let port: u16 = arg(&args, "--port").and_then(|p| p.parse().ok()).unwrap_or(8787);
    let host = arg(&args, "--host").unwrap_or_else(|| "0.0.0.0".into());
    let data = PathBuf::from(arg(&args, "--data").unwrap_or_else(|| "server-data".into()));
    let store = Store::open(data.clone()).await.expect("cannot open data directory");

    // publish key: CLI flag, else persisted in server.json, else generated once
    let settings_path = data.join("server.json");
    let mut settings: Value = tokio::fs::read(&settings_path).await.ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or(json!({}));
    let publish_key = match arg(&args, "--publish-key") {
        Some(k) => k,
        None => match settings.get("publishKey").and_then(|k| k.as_str()) {
            Some(k) => k.to_string(),
            None => {
                let k: String = (0..32).map(|_| format!("{:x}", rand::random::<u8>() % 16)).collect();
                settings["publishKey"] = json!(k);
                let _ = tokio::fs::write(&settings_path, serde_json::to_vec_pretty(&settings).unwrap()).await;
                k
            }
        },
    };

    let state: Shared = Arc::new(AppState { store, sessions: Arc::new(Sessions::default()), publish_key: publish_key.clone() });
    let app = Router::new()
        .route("/api/health", get(health))
        .route("/api/games", get(list_games).post(create_game))
        .route("/api/games/:id", get(get_game).patch(patch_game))
        .route("/api/games/:id/versions", post(publish))
        .route("/api/games/:id/versions/:n", get(version_project))
        .route("/api/games/:id/live", get(live_project).post(set_live))
        .route("/api/games/:id/datastore/:store/:key", get(ds_get).put(ds_put).delete(ds_delete))
        .route("/ws/:id", get(ws))
        .layer(DefaultBodyLimit::max(64 * 1024 * 1024))
        .layer(CorsLayer::permissive())
        .with_state(state);

    let addr: SocketAddr = format!("{host}:{port}").parse().expect("invalid --host/--port");
    let listener = tokio::net::TcpListener::bind(addr).await.unwrap_or_else(|e| panic!("cannot listen on {addr}: {e}"));
    println!("Forge Server {} listening on http://{addr}", env!("CARGO_PKG_VERSION"));
    println!("data directory: {}", data.display());
    println!("publish key: {publish_key}");
    axum::serve(listener, app)
        .with_graceful_shutdown(async { let _ = tokio::signal::ctrl_c().await; println!("shutting down"); })
        .await
        .unwrap();
}
