//! Multiplayer sessions over WebSocket.
//!
//! One session runs per (game, version). The first player becomes the *host*:
//! the host client simulates server scripts and physics and replicates the
//! world; the server relays messages, assigns ids, elects a new host when the
//! host leaves and enforces who may send what.
//!
//! Messages are JSON objects with a `t` field:
//! - client → server: `state`, `chat`, `rep` (host only), `repfull` (host only, `to`),
//!   `rpc` (`to`: "host" | "all" | player id), `ping`
//! - server → client: `welcome`, `joined`, `left`, `host`, `state`, `chat`, `rep`,
//!   `repfull`, `rpc`, `pong`, `error`

use crate::store::now_ms;
use axum::extract::ws::{Message, WebSocket};
use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};

pub const MAX_PLAYERS: usize = 32;
const MAX_MESSAGE: usize = 1024 * 1024;
const MAX_SNAPSHOT: usize = 16 * 1024 * 1024;

#[derive(Clone)]
struct Player {
    id: u32,
    name: String,
    token: String,
    joined: u64,
    tx: mpsc::UnboundedSender<Message>,
}

pub struct Session {
    pub game: String,
    pub version: u32,
    players: Mutex<Vec<Player>>,
    host: Mutex<Option<u32>>,
}

#[derive(Default)]
pub struct Sessions {
    map: Mutex<HashMap<(String, u32), Arc<Session>>>,
    next_id: AtomicU32,
}

impl Sessions {
    pub async fn player_counts(&self) -> HashMap<String, usize> {
        let mut out = HashMap::new();
        for s in self.map.lock().await.values() { *out.entry(s.game.clone()).or_insert(0) += s.players.lock().await.len(); }
        out
    }

    /// True if `token` belongs to the current host of a session of `game` (DataStore writes).
    pub async fn is_host_token(&self, game: &str, token: &str) -> bool {
        for s in self.map.lock().await.values() {
            if s.game != game { continue; }
            let host = *s.host.lock().await;
            if s.players.lock().await.iter().any(|p| p.token == token && Some(p.id) == host) { return true; }
        }
        false
    }

    pub async fn sessions_json(&self, game: &str) -> Vec<Value> {
        let mut out = vec![];
        for s in self.map.lock().await.values() {
            if s.game != game { continue; }
            let players = s.players.lock().await;
            out.push(json!({ "version": s.version, "players": players.iter().map(|p| json!({ "id": p.id, "name": p.name })).collect::<Vec<_>>(), "host": *s.host.lock().await }));
        }
        out
    }

    async fn session(&self, game: &str, version: u32) -> Arc<Session> {
        let mut map = self.map.lock().await;
        map.entry((game.to_string(), version)).or_insert_with(|| Arc::new(Session { game: game.into(), version, players: Mutex::new(vec![]), host: Mutex::new(None) })).clone()
    }

    async fn drop_if_empty(&self, s: &Arc<Session>) {
        if s.players.lock().await.is_empty() {
            self.map.lock().await.remove(&(s.game.clone(), s.version));
            println!("[session] {}@v{} closed", s.game, s.version);
        }
    }

    pub async fn run(self: Arc<Self>, socket: WebSocket, game: String, version: u32, name: String) {
        let (mut sink, mut stream) = socket.split();
        let (tx, mut rx) = mpsc::unbounded_channel::<Message>();
        let writer = tokio::spawn(async move {
            while let Some(m) = rx.recv().await { if sink.send(m).await.is_err() { break; } }
        });
        let send = |tx: &mpsc::UnboundedSender<Message>, v: Value| { let _ = tx.send(Message::Text(v.to_string())); };

        let session = self.session(&game, version).await;
        let id = self.next_id.fetch_add(1, Ordering::Relaxed) + 1;
        let token: String = (0..24).map(|_| format!("{:x}", rand::random::<u8>() % 16)).collect();
        {
            let mut players = session.players.lock().await;
            if players.len() >= MAX_PLAYERS {
                send(&tx, json!({ "t": "error", "message": "This server is full." }));
                drop(players);
                drop(tx);
                let _ = writer.await;
                return;
            }
            let me = Player { id, name: name.clone(), token: token.clone(), joined: now_ms(), tx: tx.clone() };
            let mut host = session.host.lock().await;
            if host.is_none() { *host = Some(id); }
            let roster: Vec<Value> = players.iter().chain(std::iter::once(&me)).map(|p| json!({ "id": p.id, "name": p.name })).collect();
            send(&tx, json!({ "t": "welcome", "you": id, "token": token, "host": *host, "players": roster, "game": game, "version": version }));
            for p in players.iter() { send(&p.tx, json!({ "t": "joined", "id": id, "name": name })); }
            players.push(me);
        }
        println!("[session] {} joined {}@v{} as #{}", name, game, version, id);

        let mut last_state = 0u64;
        while let Some(Ok(msg)) = stream.next().await {
            let text = match msg {
                Message::Text(t) => t,
                Message::Close(_) => break,
                _ => continue,
            };
            let mut v: Value = match serde_json::from_str(&text) { Ok(v) => v, Err(_) => continue };
            let kind = v.get("t").and_then(|t| t.as_str()).unwrap_or("").to_string();
            let limit = if kind == "repfull" { MAX_SNAPSHOT } else { MAX_MESSAGE };
            if text.len() > limit { send(&tx, json!({ "t": "error", "message": "message too large" })); continue; }
            let host = *session.host.lock().await;
            let is_host = host == Some(id);
            v["from"] = json!(id);
            let players = session.players.lock().await.clone();
            let to_others = |v: &Value| { for p in players.iter().filter(|p| p.id != id) { send(&p.tx, v.clone()); } };
            match kind.as_str() {
                "state" => {
                    let t = now_ms();
                    if t - last_state < 25 { continue; } // max 40 updates/s per player
                    last_state = t;
                    to_others(&v);
                }
                "chat" => {
                    let text: String = v.get("text").and_then(|t| t.as_str()).unwrap_or("").trim().chars().take(200).collect();
                    if text.is_empty() { continue; }
                    let out = json!({ "t": "chat", "from": id, "name": name, "text": text });
                    for p in players.iter() { send(&p.tx, out.clone()); }
                }
                "rep" if is_host => to_others(&v),
                "repfull" if is_host => {
                    if let Some(to) = v.get("to").and_then(|t| t.as_u64()) {
                        if let Some(p) = players.iter().find(|p| p.id as u64 == to) { send(&p.tx, v.clone()); }
                    }
                }
                "rpc" => {
                    match v.get("to") {
                        Some(Value::String(s)) if s == "host" => { if let Some(h) = players.iter().find(|p| Some(p.id) == host) { send(&h.tx, v.clone()); } }
                        Some(Value::String(s)) if s == "all" && is_host => to_others(&v),
                        Some(Value::Number(n)) if is_host => { if let Some(p) = players.iter().find(|p| Some(p.id as u64) == n.as_u64()) { send(&p.tx, v.clone()); } }
                        _ => {}
                    }
                }
                "ping" => send(&tx, json!({ "t": "pong", "ts": v.get("ts").cloned().unwrap_or(Value::Null) })),
                _ => {}
            }
        }

        // disconnect: remove, notify, elect a new host if needed
        {
            let mut players = session.players.lock().await;
            players.retain(|p| p.id != id);
            for p in players.iter() { send(&p.tx, json!({ "t": "left", "id": id })); }
            let mut host = session.host.lock().await;
            if *host == Some(id) {
                *host = players.iter().min_by_key(|p| p.joined).map(|p| p.id);
                if let Some(h) = *host { for p in players.iter() { send(&p.tx, json!({ "t": "host", "id": h })); } }
            }
        }
        println!("[session] #{} ({}) left {}@v{}", id, name, game, version);
        self.drop_if_empty(&session).await;
        drop(tx);
        writer.abort();
    }
}
