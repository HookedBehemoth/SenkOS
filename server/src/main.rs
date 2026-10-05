//! The store's server: the ports' catalog and the system updates, for the handheld's store and
//! downloadd, and the admin API that tools/publish-ports and tools/publish-os fill it through.
//! The ports' archives, their layers and the system images live on the CDN (Bunny, S3-compatible
//! storage behind a pull zone); this only knows their paths, hashes and sizes. What the store
//! shows of a port (thumbnail, screenshots, readme) is this server's own, in MEDIA, served under
//! /media/ by its name, which is its hash's: cached for good. TLS is the reverse proxy's (Caddy).
//!
//! The handheld's side is key files, as its port.ini is (apps/common/cfg.h reads them): one
//! "[section]" per thing, "Key=value" lines, values on one line. The admin's side is JSON.
//!
//!   GET  /v1/ports                     every port: "[<id>]" sections
//!   GET  /v1/ports/<id>                one: "[Port]", then a "[File <name>]" per layer file
//!   GET  /v1/os?channel=&build=        "[Release]" newer than `build`, else 204
//!   GET  /v1/releases                  JSON: every release, full card images included
//!   GET  /media/<name>                 a thumbnail, screenshot or readme
//!   PUT  /admin/media/<name>           one, the body as is
//!   GET/PUT/DELETE /admin/ports/<id>, /admin/layers/<file>, /admin/releases/<build>
//!                                      JSON, "Authorization: Bearer $ADMIN_TOKEN"
//!
//! Environment: LISTEN (127.0.0.1:8080), DATABASE (store.db), MEDIA (media), ADMIN_TOKEN,
//! PORTS_CDN, OS_CDN (the pull zones' URLs, e.g. https://portmaster-decl.b-cdn.net).
//!
//! A port's media are names in MEDIA; in its key file they're "/media/<name>", on this server.

use axum::{
    Json, Router,
    extract::{Path, Query, State},
    http::{HeaderMap, StatusCode, header},
    response::{IntoResponse, Response},
    routing::get,
};
use rusqlite::{Connection, OptionalExtension, params};
use serde::{Deserialize, Serialize};
use std::{
    env,
    fmt::Write,
    sync::{Arc, Mutex},
};

// ponytail: one connection behind a mutex; a pool when requests queue on it
struct App {
    db: Mutex<Connection>,
    token: String,
    ports_cdn: String,
    os_cdn: String,
    media: std::path::PathBuf,
}
type Shared = State<Arc<App>>;

const SCHEMA: &str = "
CREATE TABLE IF NOT EXISTS ports (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    porter TEXT NOT NULL,
    description TEXT NOT NULL,
    genres TEXT NOT NULL,
    arch TEXT NOT NULL,
    layers TEXT NOT NULL,
    needs TEXT NOT NULL,
    instructions TEXT NOT NULL,
    archive TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    size INTEGER NOT NULL,
    installed_size INTEGER NOT NULL,
    readme TEXT NOT NULL,
    thumbnail TEXT NOT NULL,
    screenshots TEXT NOT NULL,
    version INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS layer_files (
    file TEXT PRIMARY KEY,
    layer TEXT NOT NULL,
    path TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    size INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS layer_files_layer ON layer_files (layer);
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
";

/// A port. Lists are port.ini's: ';'-separated. Paths are the CDN's. `version` goes up with
/// each publish (its time); the handheld keeps the one it installed.
#[derive(Serialize, Deserialize, Default)]
#[serde(default)]
struct Port {
    id: String,
    name: String,
    porter: String,
    description: String,
    genres: String,
    arch: String,
    layers: String,
    needs: String,
    instructions: String,
    archive: String,
    sha256: String,
    size: i64,
    installed_size: i64,
    readme: String,
    thumbnail: String,
    screenshots: String,
    version: i64,
}

/// A layer's file (docs/ports.md): "<layer>.ini", "<layer>.<arch>.squashfs"
#[derive(Serialize, Deserialize, Default)]
#[serde(default)]
struct LayerFile {
    file: String,
    layer: String,
    path: String,
    sha256: String,
    size: i64,
}

/// A system update: the slot image (squashfs, what `ab write` takes) and the full card image
/// (.wic.zst). `build` is /etc/build's, YYYYMMDDHHMMSS.
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

const PORT_COLUMNS: &str = "id, name, porter, description, genres, arch, layers, needs, instructions, archive, \
    sha256, size, installed_size, readme, thumbnail, screenshots, version";

fn port_row(r: &rusqlite::Row) -> rusqlite::Result<Port> {
    Ok(Port {
        id: r.get(0)?,
        name: r.get(1)?,
        porter: r.get(2)?,
        description: r.get(3)?,
        genres: r.get(4)?,
        arch: r.get(5)?,
        layers: r.get(6)?,
        needs: r.get(7)?,
        instructions: r.get(8)?,
        archive: r.get(9)?,
        sha256: r.get(10)?,
        size: r.get(11)?,
        installed_size: r.get(12)?,
        readme: r.get(13)?,
        thumbnail: r.get(14)?,
        screenshots: r.get(15)?,
        version: r.get(16)?,
    })
}

const RELEASE_COLUMNS: &str =
    "build, channel, notes, slot, slot_sha256, slot_size, image, image_sha256, image_size, created";

fn release_row(r: &rusqlite::Row) -> rusqlite::Result<Release> {
    Ok(Release {
        build: r.get(0)?,
        channel: r.get(1)?,
        notes: r.get(2)?,
        slot: r.get(3)?,
        slot_sha256: r.get(4)?,
        slot_size: r.get(5)?,
        image: r.get(6)?,
        image_sha256: r.get(7)?,
        image_size: r.get(8)?,
        created: r.get(9)?,
    })
}

struct Error(StatusCode, String);
impl IntoResponse for Error {
    fn into_response(self) -> Response {
        (self.0, self.1).into_response()
    }
}
impl From<rusqlite::Error> for Error {
    fn from(e: rusqlite::Error) -> Self {
        Error(StatusCode::INTERNAL_SERVER_ERROR, e.to_string())
    }
}
type Result<T> = std::result::Result<T, Error>;

fn not_found() -> Error {
    Error(StatusCode::NOT_FOUND, "not found\n".into())
}

/// "Key=value\n": a key file's values end at the line, so line breaks become spaces
fn kv(out: &mut String, key: &str, value: impl std::fmt::Display) {
    let value = value.to_string().replace(['\r', '\n'], " ");
    let _ = writeln!(out, "{key}={value}");
}

fn url(cdn: &str, path: &str) -> String {
    if path.is_empty() { String::new() } else { format!("{}/{}", cdn.trim_end_matches('/'), path) }
}

/// Every path a list of them: "a;b;" each made a URL
fn urls(cdn: &str, paths: &str) -> String {
    paths.split(';').filter(|p| !p.is_empty()).map(|p| url(cdn, p) + ";").collect()
}

fn port_keys(out: &mut String, p: &Port, cdn: &str) {
    kv(out, "Name", &p.name);
    kv(out, "Porter", &p.porter);
    kv(out, "Description", &p.description);
    kv(out, "Genres", &p.genres);
    kv(out, "Arch", &p.arch);
    kv(out, "Layers", &p.layers);
    kv(out, "Needs", &p.needs);
    kv(out, "Instructions", &p.instructions);
    kv(out, "Version", p.version);
    kv(out, "Url", url(cdn, &p.archive));
    kv(out, "Sha256", &p.sha256);
    kv(out, "Size", p.size);
    kv(out, "InstalledSize", p.installed_size);
    kv(out, "Readme", url("/media", &p.readme));
    kv(out, "Thumbnail", url("/media", &p.thumbnail));
    kv(out, "Screenshots", urls("/media", &p.screenshots));
}

fn key_file(body: String) -> Response {
    ([(header::CONTENT_TYPE, "text/plain; charset=utf-8")], body).into_response()
}

async fn catalog(State(app): Shared) -> Result<Response> {
    let db = app.db.lock().unwrap();
    let mut q = db.prepare(&format!("SELECT {PORT_COLUMNS} FROM ports ORDER BY name COLLATE NOCASE"))?;
    let mut out = String::new();
    for p in q.query_map([], port_row)? {
        let p = p?;
        let _ = writeln!(out, "[{}]", p.id);
        port_keys(&mut out, &p, &app.ports_cdn);
        out.push('\n');
    }
    Ok(key_file(out))
}

async fn port(State(app): Shared, Path(id): Path<String>) -> Result<Response> {
    let db = app.db.lock().unwrap();
    let p = db
        .query_row(&format!("SELECT {PORT_COLUMNS} FROM ports WHERE id = ?1"), [&id], port_row)
        .optional()?
        .ok_or_else(not_found)?;
    let mut out = String::from("[Port]\n");
    kv(&mut out, "Id", &p.id);
    port_keys(&mut out, &p, &app.ports_cdn);
    let mut q = db.prepare("SELECT file, path, sha256, size FROM layer_files WHERE layer = ?1 ORDER BY file")?;
    for layer in p.layers.split(';').filter(|l| !l.is_empty()) {
        let files = q.query_map([layer], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?, r.get::<_, String>(2)?, r.get::<_, i64>(3)?))
        })?;
        for f in files {
            let (file, path, sha256, size) = f?;
            let _ = writeln!(out, "\n[File {file}]");
            kv(&mut out, "Layer", layer);
            kv(&mut out, "Url", url(&app.ports_cdn, &path));
            kv(&mut out, "Sha256", sha256);
            kv(&mut out, "Size", size);
        }
    }
    Ok(key_file(out))
}

#[derive(Deserialize)]
struct UpdateQuery {
    channel: Option<String>,
    build: Option<String>,
}

async fn os_update(State(app): Shared, Query(q): Query<UpdateQuery>) -> Result<Response> {
    let db = app.db.lock().unwrap();
    let r = db
        .query_row(
            &format!("SELECT {RELEASE_COLUMNS} FROM releases WHERE channel = ?1 AND build > ?2 ORDER BY build DESC LIMIT 1"),
            params![q.channel.as_deref().unwrap_or("stable"), q.build.as_deref().unwrap_or("")],
            release_row,
        )
        .optional()?;
    let Some(r) = r else { return Ok(StatusCode::NO_CONTENT.into_response()) };
    let mut out = String::from("[Release]\n");
    kv(&mut out, "Build", &r.build);
    kv(&mut out, "Notes", &r.notes);
    kv(&mut out, "Url", url(&app.os_cdn, &r.slot));
    kv(&mut out, "Sha256", &r.slot_sha256);
    kv(&mut out, "Size", r.slot_size);
    Ok(key_file(out))
}

async fn releases(State(app): Shared) -> Result<Json<Vec<Release>>> {
    let db = app.db.lock().unwrap();
    let mut q = db.prepare(&format!("SELECT {RELEASE_COLUMNS} FROM releases ORDER BY build DESC"))?;
    let list = q
        .query_map([], release_row)?
        .map(|r| {
            r.map(|mut r| {
                r.slot = url(&app.os_cdn, &r.slot);
                r.image = url(&app.os_cdn, &r.image);
                r
            })
        })
        .collect::<rusqlite::Result<_>>()?;
    Ok(Json(list))
}

/// Content-addressed: never changes, so cached for good
async fn media(State(app): Shared, Path(name): Path<String>) -> Result<Response> {
    safe_name(&name)?;
    let body = tokio::fs::read(app.media.join(&name)).await.map_err(|_| not_found())?;
    let kind = match name.rsplit('.').next().unwrap_or("") {
        "png" => "image/png",
        "webp" => "image/webp",
        "jpg" | "jpeg" => "image/jpeg",
        "md" => "text/markdown; charset=utf-8",
        _ => "application/octet-stream",
    };
    Ok(([(header::CONTENT_TYPE, kind), (header::CACHE_CONTROL, "public, max-age=31536000, immutable")], body)
        .into_response())
}

async fn admin_put_media(
    State(app): Shared,
    headers: HeaderMap,
    Path(name): Path<String>,
    body: axum::body::Bytes,
) -> Result<StatusCode> {
    admin(&app, &headers)?;
    safe_name(&name)?;
    let io = |e: std::io::Error| Error(StatusCode::INTERNAL_SERVER_ERROR, e.to_string());
    // Whole or not at all: written aside, then renamed
    let part = app.media.join(format!(".{name}.part"));
    tokio::fs::write(&part, &body).await.map_err(io)?;
    tokio::fs::rename(&part, app.media.join(&name)).await.map_err(io)?;
    Ok(StatusCode::NO_CONTENT)
}

/// The admin's token, compared in constant time
fn admin(app: &App, headers: &HeaderMap) -> Result<()> {
    let given = headers
        .get(header::AUTHORIZATION)
        .and_then(|v| v.to_str().ok())
        .and_then(|v| v.strip_prefix("Bearer "))
        .unwrap_or("")
        .as_bytes();
    let want = app.token.as_bytes();
    let same = given.len() == want.len() && given.iter().zip(want).fold(0, |d, (a, b)| d | (a ^ b)) == 0;
    if same && !want.is_empty() { Ok(()) } else { Err(Error(StatusCode::UNAUTHORIZED, "unauthorized\n".into())) }
}

/// Ids and file names become paths on the handheld: no separators, no dot files
fn safe_name(name: &str) -> Result<()> {
    let ok = !name.is_empty()
        && !name.starts_with('.')
        && name.chars().all(|c| c.is_ascii_alphanumeric() || "._-+".contains(c));
    if ok { Ok(()) } else { Err(Error(StatusCode::BAD_REQUEST, format!("bad name: {name}\n"))) }
}

fn sha256_hex(s: &str) -> Result<()> {
    if s.len() == 64 && s.chars().all(|c| c.is_ascii_hexdigit()) {
        Ok(())
    } else {
        Err(Error(StatusCode::BAD_REQUEST, "sha256: 64 hex digits\n".into()))
    }
}

async fn admin_ports(State(app): Shared, headers: HeaderMap) -> Result<Json<Vec<Port>>> {
    admin(&app, &headers)?;
    let db = app.db.lock().unwrap();
    let mut q = db.prepare(&format!("SELECT {PORT_COLUMNS} FROM ports ORDER BY id"))?;
    Ok(Json(q.query_map([], port_row)?.collect::<rusqlite::Result<_>>()?))
}

async fn admin_put_port(
    State(app): Shared,
    headers: HeaderMap,
    Path(id): Path<String>,
    Json(mut p): Json<Port>,
) -> Result<StatusCode> {
    admin(&app, &headers)?;
    safe_name(&id)?;
    sha256_hex(&p.sha256)?;
    p.id = id;
    let db = app.db.lock().unwrap();
    db.execute(
        &format!("INSERT OR REPLACE INTO ports ({PORT_COLUMNS}) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)"),
        params![
            p.id, p.name, p.porter, p.description, p.genres, p.arch, p.layers, p.needs, p.instructions,
            p.archive, p.sha256, p.size, p.installed_size, p.readme, p.thumbnail, p.screenshots, p.version
        ],
    )?;
    Ok(StatusCode::NO_CONTENT)
}

async fn admin_delete_port(State(app): Shared, headers: HeaderMap, Path(id): Path<String>) -> Result<StatusCode> {
    admin(&app, &headers)?;
    let n = app.db.lock().unwrap().execute("DELETE FROM ports WHERE id = ?1", [id])?;
    if n == 0 { Err(not_found()) } else { Ok(StatusCode::NO_CONTENT) }
}

async fn admin_layers(State(app): Shared, headers: HeaderMap) -> Result<Json<Vec<LayerFile>>> {
    admin(&app, &headers)?;
    let db = app.db.lock().unwrap();
    let mut q = db.prepare("SELECT file, layer, path, sha256, size FROM layer_files ORDER BY file")?;
    let list = q
        .query_map([], |r| {
            Ok(LayerFile { file: r.get(0)?, layer: r.get(1)?, path: r.get(2)?, sha256: r.get(3)?, size: r.get(4)? })
        })?
        .collect::<rusqlite::Result<_>>()?;
    Ok(Json(list))
}

async fn admin_put_layer(
    State(app): Shared,
    headers: HeaderMap,
    Path(file): Path<String>,
    Json(f): Json<LayerFile>,
) -> Result<StatusCode> {
    admin(&app, &headers)?;
    safe_name(&file)?;
    safe_name(&f.layer)?;
    sha256_hex(&f.sha256)?;
    app.db.lock().unwrap().execute(
        "INSERT OR REPLACE INTO layer_files (file, layer, path, sha256, size) VALUES (?1, ?2, ?3, ?4, ?5)",
        params![file, f.layer, f.path, f.sha256, f.size],
    )?;
    Ok(StatusCode::NO_CONTENT)
}

async fn admin_delete_layer(State(app): Shared, headers: HeaderMap, Path(file): Path<String>) -> Result<StatusCode> {
    admin(&app, &headers)?;
    let n = app.db.lock().unwrap().execute("DELETE FROM layer_files WHERE file = ?1", [file])?;
    if n == 0 { Err(not_found()) } else { Ok(StatusCode::NO_CONTENT) }
}

async fn admin_releases(State(app): Shared, headers: HeaderMap) -> Result<Json<Vec<Release>>> {
    admin(&app, &headers)?;
    let db = app.db.lock().unwrap();
    let mut q = db.prepare(&format!("SELECT {RELEASE_COLUMNS} FROM releases ORDER BY build DESC"))?;
    Ok(Json(q.query_map([], release_row)?.collect::<rusqlite::Result<_>>()?))
}

async fn admin_put_release(
    State(app): Shared,
    headers: HeaderMap,
    Path(build): Path<String>,
    Json(mut r): Json<Release>,
) -> Result<StatusCode> {
    admin(&app, &headers)?;
    if build.len() != 14 || !build.chars().all(|c| c.is_ascii_digit()) {
        return Err(Error(StatusCode::BAD_REQUEST, "build: YYYYMMDDHHMMSS\n".into()));
    }
    sha256_hex(&r.slot_sha256)?;
    r.build = build;
    if r.channel.is_empty() {
        r.channel = "stable".into();
    }
    app.db.lock().unwrap().execute(
        &format!("INSERT OR REPLACE INTO releases ({RELEASE_COLUMNS}) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)"),
        params![
            r.build, r.channel, r.notes, r.slot, r.slot_sha256, r.slot_size, r.image, r.image_sha256, r.image_size,
            r.created
        ],
    )?;
    Ok(StatusCode::NO_CONTENT)
}

async fn admin_delete_release(State(app): Shared, headers: HeaderMap, Path(build): Path<String>) -> Result<StatusCode> {
    admin(&app, &headers)?;
    let n = app.db.lock().unwrap().execute("DELETE FROM releases WHERE build = ?1", [build])?;
    if n == 0 { Err(not_found()) } else { Ok(StatusCode::NO_CONTENT) }
}

fn router(app: Arc<App>) -> Router {
    Router::new()
        .route("/v1/ports", get(catalog))
        .route("/v1/ports/{id}", get(port))
        .route("/v1/os", get(os_update))
        .route("/v1/releases", get(releases))
        .route("/media/{name}", get(media))
        .route(
            "/admin/media/{name}",
            axum::routing::put(admin_put_media).layer(axum::extract::DefaultBodyLimit::max(32 << 20)),
        )
        .route("/admin/ports", get(admin_ports))
        .route("/admin/ports/{id}", axum::routing::put(admin_put_port).delete(admin_delete_port))
        .route("/admin/layers", get(admin_layers))
        .route("/admin/layers/{file}", axum::routing::put(admin_put_layer).delete(admin_delete_layer))
        .route("/admin/releases", get(admin_releases))
        .route("/admin/releases/{build}", axum::routing::put(admin_put_release).delete(admin_delete_release))
        .with_state(app)
}

fn open(path: &str) -> rusqlite::Result<Connection> {
    let db = Connection::open(path)?;
    db.execute_batch("PRAGMA journal_mode = WAL;")?;
    db.execute_batch(SCHEMA)?;
    Ok(db)
}

#[tokio::main]
async fn main() {
    let var = |k: &str, default: &str| env::var(k).unwrap_or_else(|_| default.into());
    let token = var("ADMIN_TOKEN", "");
    if token.is_empty() {
        eprintln!("store-server: no ADMIN_TOKEN: the admin API refuses everything");
    }
    let app = Arc::new(App {
        db: Mutex::new(open(&var("DATABASE", "store.db")).expect("database")),
        token,
        ports_cdn: var("PORTS_CDN", ""),
        os_cdn: var("OS_CDN", ""),
        media: var("MEDIA", "media").into(),
    });
    std::fs::create_dir_all(&app.media).expect("media");
    let listen = var("LISTEN", "127.0.0.1:8080");
    let listener = tokio::net::TcpListener::bind(&listen).await.expect("listen");
    eprintln!("store-server: on {listen}");
    axum::serve(listener, router(app)).await.expect("serve");
}

#[cfg(test)]
mod tests {
    use super::*;
    use axum::body::{Body, to_bytes};
    use axum::http::Request;
    use tower::ServiceExt;

    async fn call(app: &Router, method: &str, uri: &str, body: Option<serde_json::Value>) -> (StatusCode, String) {
        let mut req = Request::builder().method(method).uri(uri).header("authorization", "Bearer t");
        if body.is_some() {
            req = req.header("content-type", "application/json");
        }
        let req = req.body(body.map_or(Body::empty(), |b| Body::from(b.to_string()))).unwrap();
        let res = app.clone().oneshot(req).await.unwrap();
        let status = res.status();
        (status, String::from_utf8(to_bytes(res.into_body(), 1 << 20).await.unwrap().to_vec()).unwrap())
    }

    #[tokio::test]
    async fn round_trip() {
        let app = router(Arc::new(App {
            db: Mutex::new(open(":memory:").unwrap()),
            token: "t".into(),
            ports_cdn: "https://p.example/".into(),
            os_cdn: "https://o.example".into(),
            media: std::env::temp_dir().join(format!("store-test-{}", std::process::id())),
        }));
        let sha = "a".repeat(64);
        let port = serde_json::json!({"name": "Two\nLines", "layers": "frt;", "archive": "ports/x.tar.zst",
            "sha256": sha, "screenshots": "a.png;b.png;", "version": 7});
        assert_eq!(call(&app, "PUT", "/admin/ports/x", Some(port.clone())).await.0, StatusCode::NO_CONTENT);
        assert_eq!(call(&app, "PUT", "/admin/ports/..", Some(port)).await.0, StatusCode::BAD_REQUEST);
        let layer = serde_json::json!({"layer": "frt", "path": "layers/frt.ini", "sha256": sha, "size": 3});
        assert_eq!(call(&app, "PUT", "/admin/layers/frt.ini", Some(layer)).await.0, StatusCode::NO_CONTENT);

        let (_, all) = call(&app, "GET", "/v1/ports", None).await;
        assert!(all.contains("[x]\nName=Two Lines\n"), "{all}");
        assert!(all.contains("Url=https://p.example/ports/x.tar.zst\n"), "{all}");
        assert!(all.contains("Screenshots=/media/a.png;/media/b.png;\n"), "{all}");
        let (_, one) = call(&app, "GET", "/v1/ports/x", None).await;
        assert!(one.contains("[File frt.ini]\nLayer=frt\nUrl=https://p.example/layers/frt.ini\n"), "{one}");

        let rel = serde_json::json!({"slot": "20261005000000/slot.squashfs", "slot_sha256": sha, "slot_size": 9});
        assert_eq!(call(&app, "PUT", "/admin/releases/20261005000000", Some(rel)).await.0, StatusCode::NO_CONTENT);
        let (s, up) = call(&app, "GET", "/v1/os?build=20261004000000", None).await;
        assert_eq!(s, StatusCode::OK);
        assert!(up.contains("Build=20261005000000\nNotes=\nUrl=https://o.example/20261005000000/slot.squashfs\n"), "{up}");
        assert_eq!(call(&app, "GET", "/v1/os?build=20261005000000", None).await.0, StatusCode::NO_CONTENT);

        std::fs::create_dir_all(std::env::temp_dir().join(format!("store-test-{}", std::process::id()))).unwrap();
        let put = Request::builder().method("PUT").uri("/admin/media/a.png").header("authorization", "Bearer t")
            .body(Body::from("png!")).unwrap();
        assert_eq!(app.clone().oneshot(put).await.unwrap().status(), StatusCode::NO_CONTENT);
        assert_eq!(call(&app, "GET", "/media/a.png", None).await, (StatusCode::OK, "png!".into()));
        assert_eq!(call(&app, "GET", "/media/.a.png.part", None).await.0, StatusCode::BAD_REQUEST);

        let anon = Request::builder().uri("/admin/ports").body(Body::empty()).unwrap();
        assert_eq!(app.clone().oneshot(anon).await.unwrap().status(), StatusCode::UNAUTHORIZED);
    }
}
