# Store and system updates

Ports (PortMaster-Decl's, [ports.md](ports.md)) and system updates come from a server of ours and a CDN.

```
tools/publish ──rclone──▶ Bunny storage (S3)  ──pull zone──▶  CDN  ◀── downloadd (ports, layers, system images)
      └──admin API──▶ server (server/, axum + SQLite, media on disk) ◀── downloadd, store (what there is; images, readmes)
```

Bunny holds only what gets installed: the ports' archives, their layers and the system images. What the store
shows of a port (thumbnail, screenshots, readme) is the server's own, in its `MEDIA` folder.

| Bunny storage zone | Pull zone (public) | Holds |
|---|---|---|
| `portmaster-decl` | `https://portmaster-decl.b-cdn.net` | `ports/<id>/<hash>-<id>.tar.zst`; `layers/<hash>-<file>` |
| `hare-os-images` | `https://hare-os-images.b-cdn.net` | `<build>/slot.squashfs.zst`, `<build>/anbernic-image.wic.zst`, `.wic.bmap` |

Files are named by their content's hash: a new version is a new name, so no cache ever serves an old copy.

## The server

[server/](../server): one binary, one SQLite file. TLS is a reverse proxy's (Caddy).

```
cd server && cargo build --release
ADMIN_TOKEN=… DATABASE=/var/lib/store/store.db MEDIA=/var/lib/store/media LISTEN=127.0.0.1:8080 \
  PORTS_CDN=https://portmaster-decl.b-cdn.net OS_CDN=https://hare-os-images.b-cdn.net target/release/store-server
```

```
# Caddyfile
store.example.org {
    reverse_proxy 127.0.0.1:8080
}
```

| Request | Answer |
|---|---|
| `GET /v1/ports` | every port, a key file (`[<id>]` sections: Name, Porter, Description, Genres, Arch, Layers, Needs, Instructions, Version, Url, Sha256, Size, InstalledSize, Readme, Thumbnail, Screenshots) |
| `GET /v1/ports/<id>` | `[Port]` (the same, and Id), then `[File <name>]` per file of its layers (Layer, Url, Sha256, Size) |
| `GET /v1/os?channel=stable&build=<build>` | `[Release]` Build, Notes, Url, Sha256, Size of the newest build past `build`, else 204 |
| `GET /v1/releases` | JSON: every release, its card image too (a download page) |
| `GET /media/<name>` | a port's thumbnail, screenshot or readme (Readme, Thumbnail and Screenshots are `/media/<name>` paths on this server); cached for good: a name is its content's hash |
| `PUT /admin/media/<name>` | one of those, the body as is |
| `GET`, `PUT`, `DELETE /admin/{ports/<id>, layers/<file>, releases/<build>}` | JSON, `Authorization: Bearer $ADMIN_TOKEN` |

The handheld's side is key files, like port.ini: `apps/common/cfg.h` reads them, nothing parses JSON there.
`cargo test` runs a round trip through every route.

## Publishing

`tools/publish` reads the repo's `.env` (or the environment): `PORTMASTER_ENDPOINT` (Bunny's S3 endpoint),
`PORTMASTER_BUCKET`/`PORTMASTER_PASSWORD`, `OS_BUCKET`/`OS_PASSWORD`, `STORE_ADMIN` (the server) and `ADMIN_TOKEN`.

```
tools/publish ports /mnt/multimedia/source/PortMaster-Decl/ports/{alex4,goldminer}    # with their layers
tools/publish os -m "Wi-Fi, the store"                                                 # the last image build
```

- A port: its folder as a `.tar.zst` (no top directory) to Bunny; `port.md`, its cover/screenshot and a 256 px
  WebP thumbnail of the first to the server (`<hash>-<id>-<file>`). Its `version` is the time it was published. Only the layers' `aarch64` and `armhf` files
  go up.
- A release: the slot image (`squashfs-lz4`, zstd'ed again: 174 → 142 MB) and the card image. Its build is
  `/etc/build` in the slot image (`YYYYMMDDHHMMSS`, written by the image build), which is what the handheld
  compares.

## On the handheld

`/etc/store` (root's; `/data/OS/overlay/etc/store` for another) names the server: `[Store] Server=`, `Channel=`.

**downloadd** ([apps/downloadd](../apps/downloadd/main.c), root, from `anbernic.init`) does every download, one at
a time, each in a process of its own: a port's as `player` (so what it unpacks can't reach past the player's files),
the system's as root. It listens on `/run/downloadd.sock` (root:player 0660), lines in both directions:
`install ID`, `remove ID`, `cancel ID`, `check`, `update`; `job ID STATE DONE TOTAL [MESSAGE]` (queued,
downloading, installing, done, removed, failed, canceled) and `os available BUILD SIZE NOTES | os current | os failed WHY`.
A client hears every job's state when it connects, and the answer to every check. By hand: `downloadd port ID`, `downloadd check`, ….

- A port: the server's `/v1/ports/<id>`, the layer files it needs for the first of its `Arch` the handheld runs
  (into `/data/OS/layers`, skipped when one of that name and size is there), then the archive: hashed and unpacked as it
  downloads into `apps/ports/.store-<id>`, moved to `apps/ports/<id>` (an update is copied over it, so the files the
  player brought stay), with `.store` saying its `Version`. Then "reload" on the launcher's FIFO.
- The system: the newest build past `/etc/build`, decompressed straight into the other slot (`ab write`), checked
  against its SHA-256, then armed (`ab arm`). Rebooting tries it ([dev-workflow.md](dev-workflow.md), A/B slots); once the
  launcher is up, it tells sysd ("up"), which commits it (`ab commit`). A slot that never gets there falls back.

**store** ([apps/store](../apps/store/main.c)): the catalog as tiles (All, Installed, a page per genre: L1/R1), a
port's page (screenshot, description, size, layers, what it needs; Install, Update, Cancel, Readme, Remove) and its
readme. Thumbnails, screenshots and readmes come from the server's `/media/` and are cached in `~/.cache/store` by name. The launcher's quick
menu starts it; Select on a port opens its readme or its page (`store ID`).

**Updates** (the launcher's Settings): this build, Check for updates, Download and install, the progress, then
Restart.

**Wi-Fi** (the launcher's Settings): on/off, the networks by signal, A connects (a password on the on-screen
keyboard: X deletes, Y shifts, Start is done), X forgets a saved one. See [dev-workflow.md](dev-workflow.md).
