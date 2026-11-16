//! Persistent storage: games, their immutable published versions (content
//! addressed like git objects), the "live" pointer and per-game DataStores.
//!
//! Layout below the data directory:
//! ```text
//! objects/<sha256>                    published project files (immutable)
//! games/<id>/game.json                metadata + live version
//! games/<id>/versions.json            version history (append-only)
//! games/<id>/datastore/<store>.json   DataStore key/value maps
//! server.json                         server settings (publish key)
//! ```

use serde::{Deserialize, Serialize};
use serde_json::{Map, Value};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::RwLock;

pub fn now_ms() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0)
}

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct Game {
    pub id: String,
    pub name: String,
    pub description: String,
    pub author: String,
    pub created: u64,
    pub updated: u64,
    pub live: Option<u32>,
    #[serde(default)]
    pub thumbnail: Option<String>,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "camelCase")]
pub struct Version {
    pub number: u32,
    pub hash: String,
    pub parent: Option<u32>,
    pub message: String,
    pub author: String,
    pub created: u64,
    pub size: u64,
}

struct GameRecord {
    game: Game,
    versions: Vec<Version>,
}

#[derive(Debug)]
pub enum StoreError {
    NotFound(String),
    Conflict { latest: Option<u32> },
    Invalid(String),
    Io(String),
}

impl From<std::io::Error> for StoreError {
    fn from(e: std::io::Error) -> Self { StoreError::Io(e.to_string()) }
}

pub struct Store {
    root: PathBuf,
    games: RwLock<HashMap<String, GameRecord>>,
    datastores: RwLock<HashMap<(String, String), Map<String, Value>>>,
}

/// Writes a file atomically (temp file + rename) so a crash never leaves partial data.
async fn write_atomic(path: &Path, data: &[u8]) -> std::io::Result<()> {
    if let Some(dir) = path.parent() { tokio::fs::create_dir_all(dir).await?; }
    let tmp = path.with_extension(format!("tmp{}", rand::random::<u32>()));
    tokio::fs::write(&tmp, data).await?;
    tokio::fs::rename(&tmp, path).await
}

pub fn valid_name(s: &str, max: usize) -> bool {
    !s.trim().is_empty() && s.chars().count() <= max && !s.chars().any(|c| c.is_control())
}

pub fn valid_key(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && s.chars().all(|c| c.is_ascii_alphanumeric() || "_-. ".contains(c))
}

impl Store {
    pub async fn open(root: PathBuf) -> std::io::Result<Store> {
        tokio::fs::create_dir_all(root.join("objects")).await?;
        tokio::fs::create_dir_all(root.join("games")).await?;
        let mut games = HashMap::new();
        let mut dir = tokio::fs::read_dir(root.join("games")).await?;
        while let Some(entry) = dir.next_entry().await? {
            let p = entry.path();
            let game: Game = match tokio::fs::read(p.join("game.json")).await.ok().and_then(|b| serde_json::from_slice(&b).ok()) {
                Some(g) => g,
                None => { eprintln!("[store] skipping unreadable game in {}", p.display()); continue; }
            };
            let versions: Vec<Version> = tokio::fs::read(p.join("versions.json")).await.ok()
                .and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
            games.insert(game.id.clone(), GameRecord { game, versions });
        }
        println!("[store] {} game(s) loaded from {}", games.len(), root.display());
        Ok(Store { root, games: RwLock::new(games), datastores: RwLock::new(HashMap::new()) })
    }

    fn game_dir(&self, id: &str) -> PathBuf { self.root.join("games").join(id) }

    async fn save_game(&self, rec: &GameRecord) -> Result<(), StoreError> {
        let dir = self.game_dir(&rec.game.id);
        write_atomic(&dir.join("game.json"), &serde_json::to_vec_pretty(&rec.game).unwrap()).await?;
        write_atomic(&dir.join("versions.json"), &serde_json::to_vec_pretty(&rec.versions).unwrap()).await?;
        Ok(())
    }

    pub async fn list(&self) -> Vec<(Game, usize)> {
        let g = self.games.read().await;
        let mut out: Vec<(Game, usize)> = g.values().map(|r| (r.game.clone(), r.versions.len())).collect();
        out.sort_by(|a, b| b.0.updated.cmp(&a.0.updated));
        out
    }

    pub async fn get(&self, id: &str) -> Option<(Game, Vec<Version>)> {
        self.games.read().await.get(id).map(|r| (r.game.clone(), r.versions.clone()))
    }

    pub async fn create_game(&self, name: &str, description: &str, author: &str) -> Result<Game, StoreError> {
        if !valid_name(name, 64) { return Err(StoreError::Invalid("game name must be 1-64 characters".into())); }
        let alphabet = b"abcdefghjkmnpqrstuvwxyz23456789";
        let mut games = self.games.write().await;
        let id = loop {
            let id: String = (0..8).map(|_| alphabet[rand::random::<usize>() % alphabet.len()] as char).collect();
            if !games.contains_key(&id) { break id; }
        };
        let t = now_ms();
        let game = Game { id: id.clone(), name: name.trim().into(), description: description.chars().take(500).collect(), author: author.chars().take(64).collect(), created: t, updated: t, live: None, thumbnail: None };
        let rec = GameRecord { game: game.clone(), versions: vec![] };
        self.save_game(&rec).await?;
        games.insert(id, rec);
        Ok(game)
    }

    /// Publishes a new immutable version. Like a git push it is rejected when
    /// `parent` is not the latest version (someone else published in between),
    /// unless `force` is set.
    #[allow(clippy::too_many_arguments)]
    pub async fn publish(&self, id: &str, parent: Option<u32>, message: &str, author: &str, content: &[u8], thumbnail: Option<String>, set_live: bool, force: bool) -> Result<Version, StoreError> {
        if content.is_empty() { return Err(StoreError::Invalid("empty project".into())); }
        // projects are JSON; reject anything else early
        let parsed: Value = serde_json::from_slice(content).map_err(|_| StoreError::Invalid("project is not valid JSON".into()))?;
        if parsed.get("format").and_then(|f| f.as_str()) != Some("forge-project") {
            return Err(StoreError::Invalid("not a forge-project file".into()));
        }
        let mut games = self.games.write().await;
        let rec = games.get_mut(id).ok_or_else(|| StoreError::NotFound(format!("game {id}")))?;
        let latest = rec.versions.last().map(|v| v.number);
        if !force && parent != latest { return Err(StoreError::Conflict { latest }); }
        let hash = hex::encode(Sha256::digest(content));
        let obj = self.root.join("objects").join(&hash);
        if tokio::fs::metadata(&obj).await.is_err() { write_atomic(&obj, content).await?; }
        let v = Version {
            number: latest.unwrap_or(0) + 1, hash, parent: latest,
            message: message.chars().take(500).collect(), author: author.chars().take(64).collect(),
            created: now_ms(), size: content.len() as u64,
        };
        rec.versions.push(v.clone());
        if set_live { rec.game.live = Some(v.number); }
        if let Some(t) = thumbnail.filter(|t| t.starts_with("data:image/") && t.len() < 400_000) { rec.game.thumbnail = Some(t); }
        if let Some(name) = parsed.get("name").and_then(|n| n.as_str()) { if valid_name(name, 64) && rec.versions.len() == 1 { rec.game.name = name.into(); } }
        rec.game.updated = now_ms();
        self.save_game(rec).await?;
        Ok(v)
    }

    pub async fn set_live(&self, id: &str, number: u32) -> Result<Game, StoreError> {
        let mut games = self.games.write().await;
        let rec = games.get_mut(id).ok_or_else(|| StoreError::NotFound(format!("game {id}")))?;
        if !rec.versions.iter().any(|v| v.number == number) { return Err(StoreError::NotFound(format!("version {number}"))); }
        rec.game.live = Some(number);
        rec.game.updated = now_ms();
        self.save_game(rec).await?;
        Ok(rec.game.clone())
    }

    pub async fn update_meta(&self, id: &str, name: Option<String>, description: Option<String>) -> Result<Game, StoreError> {
        let mut games = self.games.write().await;
        let rec = games.get_mut(id).ok_or_else(|| StoreError::NotFound(format!("game {id}")))?;
        if let Some(n) = name { if !valid_name(&n, 64) { return Err(StoreError::Invalid("invalid name".into())); } rec.game.name = n.trim().into(); }
        if let Some(d) = description { rec.game.description = d.chars().take(500).collect(); }
        rec.game.updated = now_ms();
        self.save_game(rec).await?;
        Ok(rec.game.clone())
    }

    pub async fn content(&self, id: &str, number: Option<u32>) -> Result<(Version, Vec<u8>), StoreError> {
        let v = {
            let games = self.games.read().await;
            let rec = games.get(id).ok_or_else(|| StoreError::NotFound(format!("game {id}")))?;
            let n = number.or(rec.game.live).ok_or_else(|| StoreError::NotFound("game has no live version".into()))?;
            rec.versions.iter().find(|v| v.number == n).cloned().ok_or_else(|| StoreError::NotFound(format!("version {n}")))?
        };
        let data = tokio::fs::read(self.root.join("objects").join(&v.hash)).await?;
        // integrity check: content must still match its hash
        if hex::encode(Sha256::digest(&data)) != v.hash { return Err(StoreError::Io(format!("object {} is corrupt", v.hash))); }
        Ok((v, data))
    }

    // ---- DataStore -------------------------------------------------------------------

    async fn load_store(&self, game: &str, store: &str) -> Map<String, Value> {
        let key = (game.to_string(), store.to_string());
        if let Some(m) = self.datastores.read().await.get(&key) { return m.clone(); }
        let path = self.game_dir(game).join("datastore").join(format!("{store}.json"));
        let m: Map<String, Value> = tokio::fs::read(&path).await.ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
        self.datastores.write().await.insert(key, m.clone());
        m
    }

    pub async fn ds_get(&self, game: &str, store: &str, key: &str) -> Result<Option<Value>, StoreError> {
        if !self.games.read().await.contains_key(game) { return Err(StoreError::NotFound(format!("game {game}"))); }
        if !valid_key(store) || !valid_key(key) { return Err(StoreError::Invalid("invalid store or key name".into())); }
        Ok(self.load_store(game, store).await.get(key).cloned())
    }

    /// Sets (or removes with `None`) a value; returns the previous value.
    pub async fn ds_set(&self, game: &str, store: &str, key: &str, value: Option<Value>) -> Result<Option<Value>, StoreError> {
        if !self.games.read().await.contains_key(game) { return Err(StoreError::NotFound(format!("game {game}"))); }
        if !valid_key(store) || !valid_key(key) { return Err(StoreError::Invalid("invalid store or key name".into())); }
        if let Some(v) = &value { if serde_json::to_vec(v).map(|b| b.len()).unwrap_or(usize::MAX) > 256 * 1024 { return Err(StoreError::Invalid("value larger than 256 KB".into())); } }
        let mut m = self.load_store(game, store).await;
        let prev = match value { Some(v) => m.insert(key.to_string(), v), None => m.remove(key) };
        let path = self.game_dir(game).join("datastore").join(format!("{store}.json"));
        write_atomic(&path, &serde_json::to_vec(&m).unwrap()).await?;
        self.datastores.write().await.insert((game.to_string(), store.to_string()), m);
        Ok(prev)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn project(name: &str) -> Vec<u8> {
        serde_json::to_vec(&serde_json::json!({ "format": "forge-project", "version": 1, "name": name, "scene": { "services": [] } })).unwrap()
    }

    #[tokio::test]
    async fn publish_is_git_like() {
        let dir = std::env::temp_dir().join(format!("forge-store-test-{}", rand::random::<u32>()));
        let store = Store::open(dir.clone()).await.unwrap();
        let g = store.create_game("Test", "", "me").await.unwrap();
        let v1 = store.publish(&g.id, None, "first", "me", &project("A"), None, true, false).await.unwrap();
        assert_eq!(v1.number, 1);
        // stale parent is rejected like a non-fast-forward push
        match store.publish(&g.id, None, "stale", "me", &project("B"), None, true, false).await {
            Err(StoreError::Conflict { latest: Some(1) }) => {}
            other => panic!("expected conflict, got {other:?}"),
        }
        let v2 = store.publish(&g.id, Some(1), "second", "me", &project("B"), None, true, false).await.unwrap();
        assert_eq!(v2.parent, Some(1));
        assert_eq!(store.get(&g.id).await.unwrap().0.live, Some(2));
        store.set_live(&g.id, 1).await.unwrap(); // rollback
        let (v, data) = store.content(&g.id, None).await.unwrap();
        assert_eq!(v.number, 1);
        assert_eq!(data, project("A"));
        // reload from disk
        let again = Store::open(dir.clone()).await.unwrap();
        assert_eq!(again.get(&g.id).await.unwrap().1.len(), 2);
        // datastore
        store.ds_set(&g.id, "Coins", "player_1", Some(serde_json::json!(5))).await.unwrap();
        assert_eq!(store.ds_get(&g.id, "Coins", "player_1").await.unwrap(), Some(serde_json::json!(5)));
        assert!(store.publish(&g.id, Some(2), "bad", "me", b"not json", None, true, true).await.is_err());
        let _ = tokio::fs::remove_dir_all(dir).await;
    }
}
