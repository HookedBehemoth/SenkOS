# System updates and game packs

System images and box art packs are files on the VPS's disk, which Caddy serves under
**https://senkos.dev/files/**. Our server there knows what there is and sends downloads to them with a 302.

```
tools/publish ──rsync──▶ VPS /home/vps/senkos/files ──Caddy──▶ senkos.dev/files/  ◀──302── server ◀── downloadd, the site
      └──admin API──▶ server (server/, axum + SQLite)
```

A build's files are in a folder named by its build (`<build>/slot.squashfs.zst`, `senkos-image.wic.gz`,
`.wic.bmap`), a pack in one named by its hash (`thumbnails/<hash>/<Pack>.pack`, its name kept: ready to copy). A path
never changes what it serves, so Caddy has them cached for good.

## The server

[server/](../server): one binary, one SQLite file. TLS is a reverse proxy's (Caddy). It runs on the VPS (deployed from
`~/source/vps/hosts/vps/senkos`, a container built from this folder): Caddy serves [site/](../site) (static) and
the files, and passes `/v1/` and the site's lists on: `/download/rg35xx-sp` (the latest card image; `.bmap`), `/releases`, `/packs`.
The admin API listens on the VPN address only: `ADMIN_URL=http://10.10.10.1:8091`.

```
cd server && cargo build --release
DATABASE=/var/lib/senkos/senkos.db LISTEN=127.0.0.1:8080 target/release/senkos-server
# ADMIN_LISTEN=10.10.10.1:8091; DOWNLOADS: the files' base URL, https://senkos.dev/files by default
```

| Request | Answer |
|---|---|
| `GET /v1/os?channel=stable&build=<build>` | `[Release]` Build, Notes, Url, Sha256, Size of the newest build past `build`, else 204 |
| `GET /v1/releases` | JSON: every release, its card image too |
| `GET /v1/packs` | every launcher game pack, a key file (`[<Pack>]` sections: Url, Sha256, Size) |
| `GET /download/rg35xx-sp[.bmap]` | 302 to the latest stable card image (or its bmap) |
| `GET`, `PUT`, `DELETE /admin/releases/<build>`, `PUT`, `DELETE /admin/packs/<Pack>` | JSON on the admin listener, without authentication |

## Publishing

`tools/publish` needs ssh to the VPS (rsync; `FILES`, `vps@10.10.10.1:/home/vps/senkos/files` by default) and
`ADMIN_URL` (environment or `.env`).

```
tools/publish os -m "Wi-Fi, box art"                                    # the last image build
tools/publish packs /mnt/multimedia/anbernic-thumbnails/out/*.pack      # tools/boxart-pack's
```

- A release: its build is `/etc/build` in the slot image (`YYYYMMDDHHMMSS`, written by the image build), which is
  what the handheld compares. The slot image (`squashfs-lz4`) goes up zstd'ed again (174 → 142 MB).

## On the handheld

`/etc/updates` (root's; `/data/OS/overlay/etc/updates` for another) names the server: `[Updates] Server=`, `Channel=`.

**downloadd** ([apps/downloadd](../apps/downloadd/main.c), root, respawned by init) does every download, one at
a time, each in a process of its own: a pack's as `player`, the system's as root. It's `org.senkos.Updates1` on
the system bus ([its interface](../apps/bus/org.senkos.Updates1.xml); shell and root may call it): `Check`
(answered once the server said), `Update`, `Pack NAME`, `Cancel ID`, `Jobs`, and a `Job` signal for each change
(queued, downloading, installing, done, failed, canceled). By hand, one job without the daemon:
`downloadd pack NAME`, `downloadd check`, `downloadd os`.

- A box art pack (a `.system` file's `Pack`, job id `pack:NAME` with `_` for spaces): its `[NAME]` in the server's
  `/v1/packs`, downloaded as the player into `OS/assets/thumbnails/NAME.pack`, checked against its SHA-256; the
  launcher reloads when its Job says done. Select on a game whose system's pack isn't on the card offers it (Download box
  art); the launcher says how it went in a toast.
- The system: the newest build past `/etc/build`, decompressed straight into the other slot (`ab write`), checked
  against its SHA-256, then armed (`ab arm`). Rebooting tries it ([dev-workflow.md](dev-workflow.md), A/B slots); once the
  launcher is up, it tells sysd ("up"), which commits it (`ab commit`). A slot that never gets there falls back.

**Updates** (the launcher's Settings): this build, Check for updates, Download and install, the progress, then
Restart.

**Wi-Fi** (the launcher's Settings): on/off, the networks by signal, A connects (a password on the on-screen
keyboard: X deletes, Y shifts, Start is done), X forgets a saved one. See [dev-workflow.md](dev-workflow.md).
