//! The update server: system updates and the launcher's game packs for downloadd, the site's
//! lists, and the admin API that tools/publish fills it through.
//! The system images and packs are files Caddy serves from the VPS's disk (DOWNLOADS); this only
//! knows their paths, hashes and sizes, and sends downloads there. TLS is Caddy's too.
//!
//! The handheld's side is key files (apps/common/cfg.h reads them): one "[section]" per thing,
//! "Key=value" lines, values on one line. The admin's side is JSON.
//!
//!   GET  /v1/os?channel=&build=        "[Release]" newer than `build`, else 204
//!   GET  /v1/releases                  JSON: every release, full card images included
//!   GET  /v1/packs                     every launcher game pack: "[<name>]" sections (Url, Sha256, Size)
//!   GET  /download/rg35xx-sp[.bmap]    302 to the latest stable card image (or its bmap)
//!   GET  /releases, /packs             the site's lists, HTML
//!   GET/PUT/DELETE /admin/releases/<build>, PUT/DELETE /admin/packs/<name>
//!                                      JSON, on the admin listener only
//!
//! Environment: LISTEN (127.0.0.1:8080), ADMIN_LISTEN (127.0.0.1:8091; the admin API's local or VPN
//! address), DATABASE (senkos.db), DOWNLOADS (the files' base URL, https://senkos.dev/files).

use axum::{
    Json, Router,
    extract::{Path, Query, State},
    http::{StatusCode, header},
    response::{IntoResponse, Response},
    routing::get,
};
use maud::{DOCTYPE, Markup, html};
use rusqlite::{Connection, OptionalExtension, params};
use serde::{Deserialize, Serialize};
use std::{
    env,
    fmt::Write,
    sync::{Arc, Mutex},
};

// One connection behind a mutex; a pool when requests queue on it
struct AppState {
    database: Mutex<Connection>,
    downloads: String,
}
type SharedState = State<Arc<AppState>>;

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS releases (
    build TEXT PRIMARY KEY,
    channel TEXT NOT NULL,
    notes TEXT NOT NULL,
    slot TEXT NOT NULL,
    slot_sha256 TEXT NOT NULL,
    slot_size INTEGER NOT NULL,
    image TEXT NOT NULL,
    image_sha256 TEXT NOT NULL,
    image_size INTEGER NOT NULL,
    created INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS packs (
    name TEXT PRIMARY KEY,
    path TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    size INTEGER NOT NULL
);
";

/// A launcher game pack (tools/boxart-pack), named as a .system file's Pack:
/// "Nintendo - Game Boy" is OS/assets/thumbnails/Nintendo - Game Boy.pack on the handheld
#[derive(Serialize, Deserialize, Default)]
#[serde(default)]
struct Pack {
    path: String,
    sha256: String,
    size: i64,
}

/// A system update: the slot image (squashfs, what `ab write` takes) and the full card image
/// (.wic.gz). `build` is /etc/build's, YYYYMMDDHHMMSS.
#[derive(Serialize, Deserialize, Default)]
#[serde(default)]
struct Release {
    build: String,
    channel: String,
    notes: String,
    slot: String,
    slot_sha256: String,
    slot_size: i64,
    image: String,
    image_sha256: String,
    image_size: i64,
    created: i64,
}

const RELEASE_COLUMNS: &str =
    "build, channel, notes, slot, slot_sha256, slot_size, image, image_sha256, image_size, created";

fn release_row(row: &rusqlite::Row) -> rusqlite::Result<Release> {
    Ok(Release {
        build: row.get(0)?,
        channel: row.get(1)?,
        notes: row.get(2)?,
        slot: row.get(3)?,
        slot_sha256: row.get(4)?,
        slot_size: row.get(5)?,
        image: row.get(6)?,
        image_sha256: row.get(7)?,
        image_size: row.get(8)?,
        created: row.get(9)?,
    })
}

struct Error(StatusCode, String);
impl IntoResponse for Error {
    fn into_response(self) -> Response {
        (self.0, self.1).into_response()
    }
}
impl From<rusqlite::Error> for Error {
    fn from(error: rusqlite::Error) -> Self {
        Error(StatusCode::INTERNAL_SERVER_ERROR, error.to_string())
    }
}
type Result<T> = std::result::Result<T, Error>;

fn not_found() -> Error {
    Error(StatusCode::NOT_FOUND, "not found\n".into())
}

/// "Key=value\n": a key file's values end at the line, so line breaks become spaces
fn write_key(output: &mut String, key: &str, value: impl std::fmt::Display) {
    let value = value.to_string().replace(['\r', '\n'], " ");
    let _ = writeln!(output, "{key}={value}");
}

fn asset_url(base_url: &str, path: &str) -> String {
    if path.is_empty() { String::new() } else { format!("{}/{}", base_url.trim_end_matches('/'), path) }
}

/// What changes with a publish: a minute for the handheld's lists, five for the site's pages
const LIST_CACHE_CONTROL: (header::HeaderName, &str) = (header::CACHE_CONTROL, "public, max-age=60");
const PAGE_CACHE_CONTROL: (header::HeaderName, &str) = (header::CACHE_CONTROL, "public, max-age=300");

fn key_file_response(body: String) -> Response {
    ([(header::CONTENT_TYPE, "text/plain; charset=utf-8"), LIST_CACHE_CONTROL], body).into_response()
}

#[derive(Deserialize)]
struct UpdateQuery {
    channel: Option<String>,
    build: Option<String>,
}

async fn os_update(State(app): SharedState, Query(query): Query<UpdateQuery>) -> Result<Response> {
    let database = app.database.lock().unwrap();
    let release = database
        .query_row(
            &format!("SELECT {RELEASE_COLUMNS} FROM releases WHERE channel = ?1 AND build > ?2 ORDER BY build DESC LIMIT 1"),
            params![query.channel.as_deref().unwrap_or("stable"), query.build.as_deref().unwrap_or("")],
            release_row,
        )
        .optional()?;
    let Some(release) = release else { return Ok(StatusCode::NO_CONTENT.into_response()) };
    let mut output = String::from("[Release]\n");
    write_key(&mut output, "Build", &release.build);
    write_key(&mut output, "Notes", &release.notes);
    write_key(&mut output, "Url", asset_url(&app.downloads, &release.slot));
    write_key(&mut output, "Sha256", &release.slot_sha256);
    write_key(&mut output, "Size", release.slot_size);
    Ok(key_file_response(output))
}

async fn releases(State(app): SharedState) -> Result<([(header::HeaderName, &'static str); 1], Json<Vec<Release>>)> {
    let database = app.database.lock().unwrap();
    let mut statement = database.prepare(&format!("SELECT {RELEASE_COLUMNS} FROM releases ORDER BY build DESC"))?;
    let list = statement
        .query_map([], release_row)?
        .map(|row| {
            row.map(|mut release| {
                release.slot = asset_url(&app.downloads, &release.slot);
                release.image = asset_url(&app.downloads, &release.image);
                release
            })
        })
        .collect::<rusqlite::Result<_>>()?;
    Ok(([LIST_CACHE_CONTROL], Json(list)))
}

async fn packs(State(app): SharedState) -> Result<Response> {
    let database = app.database.lock().unwrap();
    let mut statement = database.prepare("SELECT name, path, sha256, size FROM packs ORDER BY name")?;
    let mut output = String::new();
    for pack in statement.query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, String>(2)?, row.get::<_, i64>(3)?)))? {
        let (name, path, sha256, size) = pack?;
        let _ = writeln!(output, "[{name}]");
        write_key(&mut output, "Url", asset_url(&app.downloads, &path));
        write_key(&mut output, "Sha256", sha256);
        write_key(&mut output, "Size", size);
        output.push('\n');
    }
    Ok(key_file_response(output))
}

fn format_megabytes(bytes: i64) -> String {
    if bytes < 10_000_000 { format!("{:.1} MB", bytes as f64 / 1e6) } else { format!("{} MB", bytes / 1_000_000) }
}

/// The site's own pages (site/ is the static rest, Caddy's), straight from the database
fn html_page(title: &str, body: Markup) -> Response {
    let markup = html! {
        (DOCTYPE)
        html lang="en" {
            head {
                meta charset="utf-8";
                meta name="viewport" content="width=device-width, initial-scale=1";
                title { (title) " · SenkOS" }
                link rel="stylesheet" href="/style.css";
            }
            body {
                main {
                    p { a href="/" { "SenkOS" } }
                    h1 { (title) }
                    (body)
                }
            }
        }
    };
    ([PAGE_CACHE_CONTROL], markup).into_response()
}

fn latest_release(database: &Connection) -> rusqlite::Result<Option<Release>> {
    database.query_row(
        &format!("SELECT {RELEASE_COLUMNS} FROM releases WHERE channel = 'stable' AND image != '' ORDER BY build DESC LIMIT 1"),
        [],
        release_row,
    )
    .optional()
}

/// The latest card image, or its bmap: "rg35xx-sp" or "rg35xx-sp.bmap"
async fn download(State(app): SharedState, Path(what): Path<String>) -> Result<Response> {
    let release = latest_release(&app.database.lock().unwrap())?;
    let Some(release) = release else { return Ok((StatusCode::NOT_FOUND, html_page("No release yet", html! { p { "Coming soon." } })).into_response()) };
    let path = match what.as_str() {
        "rg35xx-sp" => release.image,
        "rg35xx-sp.bmap" => release.image.replace(".wic.gz", ".wic.bmap"),
        _ => return Err(not_found()),
    };
    Ok((StatusCode::FOUND, [(header::LOCATION, asset_url(&app.downloads, &path).as_str()), PAGE_CACHE_CONTROL]).into_response())
}

async fn releases_page(State(app): SharedState) -> Result<Response> {
    let database = app.database.lock().unwrap();
    let mut statement = database.prepare(&format!("SELECT {RELEASE_COLUMNS} FROM releases WHERE channel = 'stable' AND image != '' ORDER BY build DESC"))?;
    let rows = statement.query_map([], release_row)?;
    Ok(html_page("Releases", html! {
        div.card {
            table {
                @for release in rows {
                    @let release = release?;
                    @let build = &release.build;
                    tr {
                        td {
                            a href=(asset_url(&app.downloads, &release.image)) {
                                (&build[0..4]) "-" (&build[4..6]) "-" (&build[6..8]) " " (&build[8..10]) ":" (&build[10..12])
                            }
                            br;
                            span.small {
                                (release.notes) " · SHA-256 " (release.image_sha256)
                            }
                        }
                        td { (format_megabytes(release.image_size)) }
                    }
                }
            }
        }
    }))
}

async fn packs_page(State(app): SharedState) -> Result<Response> {
    let database = app.database.lock().unwrap();
    let mut statement = database.prepare("SELECT name, path, size FROM packs ORDER BY name")?;
    let rows = statement.query_map([], |row| Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?, row.get::<_, i64>(2)?)))?;
    Ok(html_page("Box art packs", html! {
        p.small {
            "Titles, details and box art for a system's games. Copy one to "
            code { "OS/assets/thumbnails/" }
            " on the card, or press Select on the system's page on the handheld."
        }
        div.card {
            table {
                @for pack in rows {
                    @let (name, path, size) = pack?;
                    tr {
                        td { a href=(asset_url(&app.downloads, &path)) { (name) } }
                        td { (format_megabytes(size)) }
                    }
                }
            }
        }
    }))
}

/// A pack's name is a file name on the handheld and a key file's section: spaces are fine
fn safe_pack(name: &str) -> Result<()> {
    let ok = !name.is_empty() && !name.starts_with('.') && !name.chars().any(|c| c.is_control() || "/[]".contains(c));
    if ok { Ok(()) } else { Err(Error(StatusCode::BAD_REQUEST, format!("bad pack name: {name}\n"))) }
}

fn sha256_hex(hash: &str) -> Result<()> {
    if hash.len() == 64 && hash.chars().all(|c| c.is_ascii_hexdigit()) {
        Ok(())
    } else {
        Err(Error(StatusCode::BAD_REQUEST, "sha256: 64 hex digits\n".into()))
    }
}

async fn admin_put_pack(
    State(app): SharedState,
    Path(name): Path<String>,
    Json(pack): Json<Pack>,
) -> Result<StatusCode> {
    safe_pack(&name)?;
    sha256_hex(&pack.sha256)?;
    app.database.lock().unwrap().execute(
        "INSERT OR REPLACE INTO packs (name, path, sha256, size) VALUES (?1, ?2, ?3, ?4)",
        params![name, pack.path, pack.sha256, pack.size],
    )?;
    Ok(StatusCode::NO_CONTENT)
}

async fn admin_delete_pack(State(app): SharedState, Path(name): Path<String>) -> Result<StatusCode> {
    let deleted_rows = app.database.lock().unwrap().execute("DELETE FROM packs WHERE name = ?1", [name])?;
    if deleted_rows == 0 { Err(not_found()) } else { Ok(StatusCode::NO_CONTENT) }
}

async fn admin_releases(State(app): SharedState) -> Result<Json<Vec<Release>>> {
    let database = app.database.lock().unwrap();
    let mut statement = database.prepare(&format!("SELECT {RELEASE_COLUMNS} FROM releases ORDER BY build DESC"))?;
    Ok(Json(statement.query_map([], release_row)?.collect::<rusqlite::Result<_>>()?))
}

async fn admin_put_release(
    State(app): SharedState,
    Path(build): Path<String>,
    Json(mut release): Json<Release>,
) -> Result<StatusCode> {
    if build.len() != 14 || !build.chars().all(|c| c.is_ascii_digit()) {
        return Err(Error(StatusCode::BAD_REQUEST, "build: YYYYMMDDHHMMSS\n".into()));
    }
    sha256_hex(&release.slot_sha256)?;
    release.build = build;
    if release.channel.is_empty() {
        release.channel = "stable".into();
    }
    app.database.lock().unwrap().execute(
        &format!("INSERT OR REPLACE INTO releases ({RELEASE_COLUMNS}) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"),
        params![
            release.build, release.channel, release.notes, release.slot, release.slot_sha256, release.slot_size, release.image, release.image_sha256, release.image_size,
            release.created
        ],
    )?;
    Ok(StatusCode::NO_CONTENT)
}

async fn admin_delete_release(State(app): SharedState, Path(build): Path<String>) -> Result<StatusCode> {
    let deleted_rows = app.database.lock().unwrap().execute("DELETE FROM releases WHERE build = ?1", [build])?;
    if deleted_rows == 0 { Err(not_found()) } else { Ok(StatusCode::NO_CONTENT) }
}

fn public_router(app: Arc<AppState>) -> Router {
    Router::new()
        .route("/v1/os", get(os_update))
        .route("/v1/releases", get(releases))
        .route("/v1/packs", get(packs))
        .route("/download/{what}", get(download))
        .route("/releases", get(releases_page))
        .route("/packs", get(packs_page))
        .with_state(app)
}

fn admin_router(app: Arc<AppState>) -> Router {
    Router::new()
        .route("/admin/packs/{name}", axum::routing::put(admin_put_pack).delete(admin_delete_pack))
        .route("/admin/releases", get(admin_releases))
        .route("/admin/releases/{build}", axum::routing::put(admin_put_release).delete(admin_delete_release))
        .with_state(app)
}

fn open_database(path: &str) -> rusqlite::Result<Connection> {
    let database = Connection::open(path)?;
    database.execute_batch("PRAGMA journal_mode = WAL;")?;
    database.execute_batch(SCHEMA)?;
    Ok(database)
}

#[tokio::main]
async fn main() {
    let var = |k: &str, default: &str| env::var(k).unwrap_or_else(|_| default.into());
    let app = Arc::new(AppState {
        database: Mutex::new(open_database(&var("DATABASE", "senkos.db")).expect("database")),
        downloads: var("DOWNLOADS", "https://senkos.dev/files"),
    });
    let listen = var("LISTEN", "127.0.0.1:8080");
    let listener = tokio::net::TcpListener::bind(&listen).await.expect("listen");
    let admin_listen = var("ADMIN_LISTEN", "127.0.0.1:8091");
    let admin_listener = tokio::net::TcpListener::bind(&admin_listen).await.expect("admin listen");
    eprintln!("senkos-server: on {listen}, admin on {admin_listen}");
    let (public_result, admin_result) = tokio::join!(axum::serve(listener, public_router(app.clone())), axum::serve(admin_listener, admin_router(app)));
    public_result.and(admin_result).expect("serve");
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::body::{Body, to_bytes};
    use axum::http::Request;
    use tower::ServiceExt;

    async fn call(app: &Router, method: &str, uri: &str, body: Option<serde_json::Value>) -> (StatusCode, String) {
        let mut req = Request::builder().method(method).uri(uri);
        if body.is_some() {
            req = req.header("content-type", "application/json");
        }
        let req = req.body(body.map_or(Body::empty(), |b| Body::from(b.to_string()))).unwrap();
        let res = app.clone().oneshot(req).await.unwrap();
        let status = res.status();
        (status, String::from_utf8(to_bytes(res.into_body(), 1 << 20).await.unwrap().to_vec()).unwrap())
    }

    fn state() -> Arc<AppState> {
        Arc::new(AppState { database: Mutex::new(open_database(":memory:").unwrap()), downloads: "https://o.example".into() })
    }

    #[tokio::test]
    async fn round_trip() {
        let state = state();
        let app = public_router(state.clone());
        let admin = admin_router(state);
        let sha = "a".repeat(64);

        let rel = serde_json::json!({"slot": "20261005000000/slot.squashfs", "slot_sha256": sha, "slot_size": 9});
        assert_eq!(call(&admin, "PUT", "/admin/releases/20261005000000", Some(rel)).await.0, StatusCode::NO_CONTENT);
        let (s, up) = call(&app, "GET", "/v1/os?build=20261004000000", None).await;
        assert_eq!(s, StatusCode::OK);
        assert!(up.contains("Build=20261005000000\nNotes=\nUrl=https://o.example/20261005000000/slot.squashfs\n"), "{up}");
        assert_eq!(call(&app, "GET", "/v1/os?build=20261005000000", None).await.0, StatusCode::NO_CONTENT);

        let pack = serde_json::json!({"path": "thumbnails/x.pack", "sha256": sha, "size": 5});
        assert_eq!(call(&admin, "PUT", "/admin/packs/Sega%20-%20Game%20Gear", Some(pack.clone())).await.0, StatusCode::NO_CONTENT);
        assert_eq!(call(&admin, "PUT", "/admin/packs/..%2Fx", Some(pack)).await.0, StatusCode::BAD_REQUEST);
        let (_, packs) = call(&app, "GET", "/v1/packs", None).await;
        assert!(packs.contains("[Sega - Game Gear]\nUrl=https://o.example/thumbnails/x.pack\n"), "{packs}");

        assert!(call(&app, "GET", "/packs", None).await.1.contains(">Sega - Game Gear</a>"));
        assert_eq!(call(&app, "GET", "/download/rg35xx-sp", None).await.0, StatusCode::NOT_FOUND);
        let rel = serde_json::json!({"slot": "s", "slot_sha256": sha, "image": "20261006000000/senkos-image.wic.gz"});
        assert_eq!(call(&admin, "PUT", "/admin/releases/20261006000000", Some(rel)).await.0, StatusCode::NO_CONTENT);
        let dl = Request::builder().uri("/download/rg35xx-sp.bmap").body(Body::empty()).unwrap();
        let res = app.clone().oneshot(dl).await.unwrap();
        assert_eq!(res.headers()[header::LOCATION], "https://o.example/20261006000000/senkos-image.wic.bmap");

        let cached = Request::builder().uri("/v1/packs").body(Body::empty()).unwrap();
        assert_eq!(app.clone().oneshot(cached).await.unwrap().headers()[header::CACHE_CONTROL], "public, max-age=60");

        assert_eq!(call(&admin, "GET", "/admin/releases", None).await.0, StatusCode::OK);
        assert_eq!(call(&app, "GET", "/admin/releases", None).await.0, StatusCode::NOT_FOUND);
        assert_eq!(call(&app, "PUT", "/admin/packs/x", None).await.0, StatusCode::NOT_FOUND);
        assert_eq!(call(&admin, "GET", "/v1/packs", None).await.0, StatusCode::NOT_FOUND);
    }

    #[tokio::test]
    async fn pages_escape_text_and_attributes() {
        let state = state();
        let app = public_router(state.clone());
        let admin = admin_router(state);
        let sha = "a".repeat(64);
        let text = "<tag> & \"quoted\"";
        let escaped = "&lt;tag&gt; &amp; &quot;quoted&quot;";
        let pack = serde_json::json!({"path": format!("{text}.pack"), "sha256": sha, "size": 12_000_000});
        assert_eq!(call(&admin, "PUT", "/admin/packs/%3Ctag%3E%20%26%20%22quoted%22", Some(pack)).await.0, StatusCode::NO_CONTENT);
        let (_, body) = call(&app, "GET", "/packs", None).await;
        assert!(body.contains(&format!("href=\"https://o.example/{escaped}.pack\">{escaped}</a>")), "{body}");
        assert!(body.contains("<td>12 MB</td>"), "{body}");
        assert!(!body.contains("<tag>"), "{body}");

        let rel = serde_json::json!({
            "slot": "s", "slot_sha256": sha, "notes": text,
            "image": format!("{text}.wic.gz"), "image_sha256": text, "image_size": 12_000_000
        });
        assert_eq!(call(&admin, "PUT", "/admin/releases/20261006010203", Some(rel)).await.0, StatusCode::NO_CONTENT);
        let request = Request::builder().uri("/releases").body(Body::empty()).unwrap();
        let response = app.oneshot(request).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);
        assert_eq!(response.headers()[header::CONTENT_TYPE], "text/html; charset=utf-8");
        assert_eq!(response.headers()[header::CACHE_CONTROL], "public, max-age=300");
        let body = String::from_utf8(to_bytes(response.into_body(), 1 << 20).await.unwrap().to_vec()).unwrap();
        assert!(body.contains(&format!("href=\"https://o.example/{escaped}.wic.gz\">2026-10-06 01:02</a>")), "{body}");
        assert!(body.contains(&format!("<span class=\"small\">{escaped} · SHA-256 ")), "{body}");
        assert!(body.contains(&format!("SHA-256 {escaped}</span>")), "{body}");
        assert!(!body.contains("<tag>"), "{body}");
    }
}
