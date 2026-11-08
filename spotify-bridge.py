#!/usr/bin/env python3
"""Now-playing for the Aviato screen, on this machine only.

The public site stored the Spotify refresh token in Vercel Blob. That store is
blocked, so every screen poll came back as "not listening" and the album view
never opened. This process talks to Spotify directly and never calls Blob.
Bind address is loopback. The refresh token stays in a mode-600 file.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import secrets
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HOST = "127.0.0.1"
PORT = 47321
REDIRECT_URI = f"http://{HOST}:{PORT}/callback"
SCOPE = "user-read-currently-playing"
CONFIG_DIR = Path.home() / ".config" / "aviato-screen"
ENV_PATH = CONFIG_DIR / "spotify.env"
TOKEN_PATH = CONFIG_DIR / "spotify-refresh-token"
PENDING_PATH = CONFIG_DIR / "spotify-oauth-pending"
FETCH_TIMEOUT = 8
# How long a Spotify now-playing answer may be reused. The playhead below is
# moved forward with the clock, so this wait does not freeze the lyrics. It
# does delay noticing a pause, skip, or seek, so it stays short.
PLAYBACK_TTL = 4

access_cache = {"value": "", "until": 0.0}
# payload is the Spotify sample. progress_ms is that sample's position, and
# sampled_at is time.monotonic() when that position was true. Serving the
# payload unchanged made every later poll look current while the song had
# already moved on, which is why the line trailed the vocal.
playback_cache = {
    "payload": None,
    "until": 0.0,
    "progress_ms": 0,
    "duration_ms": 0,
    "sampled_at": 0.0,
}


def load_env() -> dict[str, str]:
    values: dict[str, str] = {}
    for line in ENV_PATH.read_text().splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values


def read_refresh_token() -> str:
    try:
        return TOKEN_PATH.read_text().strip()
    except OSError:
        return ""


def write_secret(path: Path, text: str) -> None:
    CONFIG_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_text(text)
    os.chmod(path, 0o600)


def offline() -> dict:
    return {
        "isLive": False,
        "title": "Not listening right now",
        "artist": "Spotify offline",
        "album": "Live playback appears here when connected",
        "albumArt": "",
        "spotifyUrl": "https://open.spotify.com",
        "progressMs": 0,
        "durationMs": 0,
        "fetchedAt": int(time.time() * 1000),
        "syncedLyrics": "",
        "status": "Spotify offline",
    }


def spotify_request(
    url: str,
    method: str,
    headers: dict,
    body: bytes | None = None,
    timeout: float = FETCH_TIMEOUT,
):
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            payload = response.read()
            return response.status, dict(response.headers), payload
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def access_token(env: dict[str, str], refresh_token: str) -> str:
    now = time.time()
    if access_cache["value"] and access_cache["until"] > now:
        return access_cache["value"]
    basic = base64.b64encode(
        f"{env['SPOTIFY_CLIENT_ID']}:{env['SPOTIFY_CLIENT_SECRET']}".encode()
    ).decode()
    status, _headers, payload = spotify_request(
        "https://accounts.spotify.com/api/token",
        "POST",
        {
            "Authorization": f"Basic {basic}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        urllib.parse.urlencode(
            {"grant_type": "refresh_token", "refresh_token": refresh_token}
        ).encode(),
    )
    if status != 200:
        return ""
    token = json.loads(payload)
    value = token.get("access_token")
    if not isinstance(value, str):
        return ""
    expires_in = token.get("expires_in") if isinstance(token.get("expires_in"), int) else 3600
    # Refresh a minute early so a poll does not land on an expired token.
    access_cache["value"] = value
    access_cache["until"] = now + max(expires_in - 60, 60)
    return value


def shape_playback(raw: dict) -> dict:
    item = raw.get("item") or {}
    if not raw.get("is_playing") or not item:
        return offline()
    images = (item.get("album") or {}).get("images") or []
    artists = item.get("artists") or []
    return {
        "isLive": True,
        "title": item.get("name") or "Unknown track",
        "artist": ", ".join(artist.get("name", "") for artist in artists if artist.get("name"))
        or "Unknown artist",
        "album": (item.get("album") or {}).get("name") or "Unknown album",
        "albumArt": images[0].get("url", "") if images else "",
        "spotifyUrl": (item.get("external_urls") or {}).get("spotify") or "https://open.spotify.com",
        "progressMs": raw.get("progress_ms") or 0,
        "durationMs": item.get("duration_ms") or 0,
        "fetchedAt": int(time.time() * 1000),
        "status": "Live on Spotify",
    }


# Spotify's Web API does not send lyrics. LRCLIB is a free synced-lyric lookup,
# one request when the track changes, then held until that song ends.
# The screen keeps 400 lines. Stopping at 12000 characters, or at 80 unsynced
# lines, left the last kept line on the panel for the rest of the song.
LYRIC_MAX = 400
LYRIC_LIMIT = 40000
LYRIC_TIMEOUT = 4
lyrics_cache = {"key": "", "synced": "", "until": 0.0}


def lyric_key(shaped: dict) -> str:
    return f"{shaped.get('title', '')}\n{shaped.get('artist', '')}\n{shaped.get('durationMs', 0)}"


def lrclib_get(url: str):
    # A timeout or a refused connection must not take down the now-playing poll.
    try:
        return spotify_request(
            url,
            "GET",
            {"User-Agent": "aviato-screen (local framebuffer lyrics)"},
            timeout=LYRIC_TIMEOUT,
        )
    except (urllib.error.URLError, TimeoutError, OSError):
        return 0, {}, b""


def trim_lrc(text: str) -> str:
    text = text.replace("\r\n", "\n").strip()
    if len(text) <= LYRIC_LIMIT:
        return text
    cut = text.rfind("\n", 0, LYRIC_LIMIT)
    return text[: cut if cut > 0 else LYRIC_LIMIT]


def plain_as_lrc(plain: str, duration_s: int) -> str:
    # No timestamps from the source. Spread the lines we keep across the whole
    # song. The old code picked a step from every line, then emitted only the
    # first 80, so that 80th line stayed up for the rest of the track.
    lines = [line.strip() for line in plain.splitlines() if line.strip()]
    if not lines or duration_s <= 0:
        return ""
    if len(lines) > LYRIC_MAX:
        lines = lines[:LYRIC_MAX]
    step = duration_s * 1000 / len(lines)
    rows = []
    for index, line in enumerate(lines):
        total = int(index * step)
        minutes, remain = divmod(total, 60000)
        seconds, fraction = divmod(remain, 1000)
        rows.append(f"[{minutes:02d}:{seconds:02d}.{fraction:03d}] {line}")
    return trim_lrc("\n".join(rows))


def lrc_from_record(record: dict, duration_s: int) -> str:
    synced = record.get("syncedLyrics")
    if isinstance(synced, str) and synced.strip():
        return trim_lrc(synced)
    plain = record.get("plainLyrics")
    if isinstance(plain, str) and plain.strip():
        return plain_as_lrc(plain, duration_s)
    return ""


def closest_lrc(payload: bytes, duration_s: int) -> str:
    try:
        parsed = json.loads(payload)
    except json.JSONDecodeError:
        return ""
    if not isinstance(parsed, list):
        return ""
    best = ""
    best_delta = 10**9
    for item in parsed:
        if not isinstance(item, dict):
            continue
        text = lrc_from_record(item, duration_s)
        if not text:
            continue
        duration = item.get("duration") if isinstance(item.get("duration"), (int, float)) else 0
        delta = abs(int(duration) - duration_s) if duration_s else 0
        if delta < best_delta:
            best = text
            best_delta = delta
    # A hit more than a few seconds off is a different recording.
    if duration_s and best_delta > 8:
        return ""
    return best


def fetch_synced_lyrics(shaped: dict) -> tuple[str, float]:
    duration_s = int(round((shaped.get("durationMs") or 0) / 1000))
    exact = urllib.parse.urlencode(
        {
            "track_name": shaped.get("title") or "",
            "artist_name": shaped.get("artist") or "",
            "album_name": shaped.get("album") or "",
            "duration": str(duration_s),
        }
    )
    status, _headers, payload = lrclib_get(f"https://lrclib.net/api/get?{exact}")
    if status == 200:
        try:
            record = json.loads(payload)
        except json.JSONDecodeError:
            record = {}
        if isinstance(record, dict):
            return lrc_from_record(record, duration_s), 86400
        return "", 120
    if status == 404:
        search = urllib.parse.urlencode(
            {
                "track_name": shaped.get("title") or "",
                "artist_name": shaped.get("artist") or "",
            }
        )
        found, _headers, body = lrclib_get(f"https://lrclib.net/api/search?{search}")
        if found == 200:
            # A definite miss stays quiet for hours. Do not look the song up again.
            return closest_lrc(body, duration_s), 6 * 3600
        return "", 120
    return "", 120


def playing_now(payload: dict | None) -> dict | None:
    """Return payload with progressMs moved to this instant.

    The cached document keeps the original sample. Adding the elapsed time on
    the way out means a second request does not rewind the playhead, and a
    lyrics lookup that takes a second does not leave that second out.
    """
    if not payload or not payload.get("isLive"):
        return payload
    elapsed = int((time.monotonic() - playback_cache["sampled_at"]) * 1000)
    if elapsed < 0:
        elapsed = 0
    progress = playback_cache["progress_ms"] + elapsed
    duration = playback_cache["duration_ms"] or 0
    if duration and progress > duration:
        progress = duration
    shown = dict(payload)
    shown["progressMs"] = progress
    shown["fetchedAt"] = int(time.time() * 1000)
    return shown


def remember_playback(shaped: dict, progress_ms: int, duration_ms: int, sampled_at: float) -> dict:
    playback_cache["payload"] = shaped
    playback_cache["progress_ms"] = progress_ms
    playback_cache["duration_ms"] = duration_ms
    playback_cache["sampled_at"] = sampled_at
    playback_cache["until"] = time.time() + PLAYBACK_TTL
    return playing_now(shaped) or shaped


def with_lyrics(shaped: dict) -> dict:
    if not shaped.get("isLive"):
        shaped["syncedLyrics"] = ""
        return shaped
    key = lyric_key(shaped)
    now = time.time()
    if lyrics_cache["key"] != key or lyrics_cache["until"] <= now:
        synced, hold = fetch_synced_lyrics(shaped)
        lyrics_cache["key"] = key
        lyrics_cache["synced"] = synced
        lyrics_cache["until"] = now + hold
    shaped["syncedLyrics"] = lyrics_cache["synced"]
    return shaped


def current_playback(env: dict[str, str]) -> dict:
    now = time.time()
    if playback_cache["payload"] is not None and playback_cache["until"] > now:
        return playing_now(playback_cache["payload"]) or offline()
    refresh_token = read_refresh_token()
    if not refresh_token:
        return offline()
    token = access_token(env, refresh_token)
    if not token:
        return offline()
    status, headers, payload = spotify_request(
        "https://api.spotify.com/v1/me/player/currently-playing?additional_types=track",
        "GET",
        {"Authorization": f"Bearer {token}"},
    )
    # The position in the body was true when Spotify wrote it, which is this
    # moment, not after the lyrics lookup below.
    sampled_at = time.monotonic()
    if status == 429:
        retry_after = headers.get("Retry-After") or headers.get("retry-after") or "30"
        try:
            wait = min(max(int(retry_after), 1), 900)
        except ValueError:
            wait = 30
        playback_cache["until"] = now + wait
        return playing_now(playback_cache["payload"]) or offline()
    if status == 204 or status == 401 or status >= 300 or not payload:
        if status == 401:
            access_cache["value"] = ""
            access_cache["until"] = 0
        shaped = offline()
        progress_ms = 0
        duration_ms = 0
    else:
        shaped = shape_playback(json.loads(payload))
        progress_ms = int(shaped.get("progressMs") or 0)
        duration_ms = int(shaped.get("durationMs") or 0)
    shaped = with_lyrics(shaped)
    return remember_playback(shaped, progress_ms, duration_ms, sampled_at)


def pkce_challenge(verifier: str) -> str:
    digest = hashlib.sha256(verifier.encode()).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode()


def authorize_url(env: dict[str, str]) -> str:
    state = secrets.token_urlsafe(24)
    verifier = secrets.token_urlsafe(64)
    write_secret(PENDING_PATH, json.dumps({"state": state, "verifier": verifier}))
    query = urllib.parse.urlencode(
        {
            "client_id": env["SPOTIFY_CLIENT_ID"],
            "code_challenge": pkce_challenge(verifier),
            "code_challenge_method": "S256",
            "redirect_uri": REDIRECT_URI,
            "response_type": "code",
            "scope": SCOPE,
            "state": state,
        }
    )
    return f"https://accounts.spotify.com/authorize?{query}"


def exchange_code(env: dict[str, str], code: str, verifier: str) -> str:
    basic = base64.b64encode(
        f"{env['SPOTIFY_CLIENT_ID']}:{env['SPOTIFY_CLIENT_SECRET']}".encode()
    ).decode()
    status, _headers, payload = spotify_request(
        "https://accounts.spotify.com/api/token",
        "POST",
        {
            "Authorization": f"Basic {basic}",
            "Content-Type": "application/x-www-form-urlencoded",
        },
        urllib.parse.urlencode(
            {
                "client_id": env["SPOTIFY_CLIENT_ID"],
                "code": code,
                "code_verifier": verifier,
                "grant_type": "authorization_code",
                "redirect_uri": REDIRECT_URI,
            }
        ).encode(),
    )
    if status != 200:
        return ""
    token = json.loads(payload)
    refresh = token.get("refresh_token")
    return refresh if isinstance(refresh, str) else ""


class Handler(BaseHTTPRequestHandler):
    env: dict[str, str] = {}

    def log_message(self, fmt: str, *args) -> None:
        # Access logs would record the OAuth code. Keep the journal quiet.
        return

    def send_json(self, payload: dict, status: int = 200) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, html: str, status: int = 200) -> None:
        body = html.encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        path = urllib.parse.urlsplit(self.path)
        if path.path == "/api/spotify":
            self.send_json(current_playback(self.env))
            return
        if path.path == "/connect":
            self.send_response(302)
            self.send_header("Location", authorize_url(self.env))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        if path.path == "/callback":
            self.finish_callback(path.query)
            return
        self.send_html("<p>Spotify bridge is running.</p>", 404)

    def finish_callback(self, query: str) -> None:
        params = urllib.parse.parse_qs(query)
        code = (params.get("code") or [""])[0]
        state = (params.get("state") or [""])[0]
        try:
            pending = json.loads(PENDING_PATH.read_text())
        except (OSError, json.JSONDecodeError):
            pending = {}
        if not code or not state or state != pending.get("state"):
            self.send_html("<p>Spotify connection did not finish.</p>", 400)
            return
        refresh = exchange_code(self.env, code, pending.get("verifier", ""))
        PENDING_PATH.unlink(missing_ok=True)
        if not refresh:
            self.send_html("<p>Spotify did not return a refresh token.</p>", 400)
            return
        write_secret(TOKEN_PATH, refresh + "\n")
        access_cache["value"] = ""
        playback_cache["payload"] = None
        playback_cache["until"] = 0
        self.send_html("<p>Spotify is connected on this machine. You can close this tab.</p>")


def main() -> None:
    Handler.env = load_env()
    if not Handler.env.get("SPOTIFY_CLIENT_ID") or not Handler.env.get("SPOTIFY_CLIENT_SECRET"):
        raise SystemExit(f"missing Spotify client credentials in {ENV_PATH}")
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"spotify bridge http://{HOST}:{PORT}/api/spotify", flush=True)
    if not read_refresh_token():
        print(f"connect at http://{HOST}:{PORT}/connect", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
