# Chatterino9 mod tracker

Counts the moderation actions and the emotes used in one channel around the
clock on a server, so the Mod activity and Emote stats windows have numbers
for the time Chatterino9 was closed.

- `server.mjs`: the service. Node 22, no dependencies. Signs in with Twitch's
  device sign-in using read-only moderator scopes, listens to
  `channel.moderate` over EventSub WebSocket and writes the same
  `actions-YYYY-MM.tsv` and `streams.tsv` files the app keeps. Reads the
  channel's chat as an anonymous viewer (no login) and counts each emote once
  per message in `emotes/YYYY-MM-DD.tsv`, looking emotes up in the 7TV, BTTV
  and FFZ lists like the app does. Serves all of it read-only on port 7480 to
  requests with the `X-Modtracker-Key` header.
- `compose.yaml`: the service plus a Cloudflare tunnel container on their own
  Docker network. No ports are published.
- `setup.ps1`: installs it from a Windows PC over one SSH connection per step
  (`install`, `start`, `update`, `signin`, `stop`, `status`, `check`) and saves the app's keys to
  `%APPDATA%\Chatterino2\Misc\ModActivity\remote.json`.

The app (`src/util/ModActivity.cpp`) reads `remote.json`, pulls new actions
when the Mod activity window opens and every 3 minutes while it is open, and
merges them into the local files. An action the app saw itself within 5
seconds of the server's copy counts once.

Endpoints, all behind Cloudflare Access and the API key:

- `GET /v1/status`: `{channel, signedIn, needsSignIn, connected, lastEventAt, serverTime, tracking: {state, until, since}, emotes: {connected, lastMessageAt, listsLoadedAt}}`
- `GET /v1/<channel>/actions?since=<msecs>`: action lines newer than `since`
- `GET /v1/<channel>/streams`: `streams.tsv`
- `GET /v1/<channel>/emotes/days`: one line per counted UTC day, `YYYY-MM-DD<TAB>mtime`
- `GET /v1/<channel>/emotes/day?d=YYYY-MM-DD`: that day's emote counts
- `POST /v1/<channel>/tracking` with `{"state": "on" | "paused" | "stopped", "until": msecs}`:
  pauses (until `until`, at most 24 hours), stops or starts counting mod
  actions. Kept in `tracking.json`. Emotes and streams are always counted.
- `POST /v1/<channel>/delete` with `{"confirm": "<channel>"}`: deletes every
  counted mod action and the stream list, answers `{actions}`

The Emote stats window (`src/util/EmoteStats.cpp`) downloads the days that
changed when it opens and every 3 minutes while it is open, and merges them
with what the app counted itself: per hour and emote the higher count and
all chatters seen.

The Mod activity window's "..." menu pauses, stops and starts the counting
and deletes the counts, on the PC and the server together. A pause, stop or
start the server did not get is sent again on the next sync. Deleting asks
the server first and deletes nothing on the PC when the server can't be
reached.

To pause: `setup.ps1 stop` (clears the counted mod actions, keeps the
sign-in and the emote stats). `setup.ps1 start` is a fresh start: it clears
papaplatte's mod action counts on the server and on the PC, then counts from
that moment. Emote stats are never cleared by the script. When the app says the
server needs a new Twitch sign-in, `setup.ps1 signin` shows the code and
keeps the counts. To remove it for good: `docker compose down` in
`~/apps/modtracker`, then revoke
the app at twitch.tv/settings/connections.
