# Chatterino9 mod tracker

Counts the moderation actions of one channel around the clock on a server,
so the Mod activity window has numbers for the time Chatterino9 was closed.

- `server.mjs`: the service. Node 22, no dependencies. Signs in with Twitch's
  device sign-in using read-only moderator scopes, listens to
  `channel.moderate` over EventSub WebSocket and writes the same
  `actions-YYYY-MM.tsv` and `streams.tsv` files the app keeps. Serves them
  read-only on port 7480 to requests with the `X-Modtracker-Key` header.
- `compose.yaml`: the service plus a Cloudflare tunnel container on their own
  Docker network. No ports are published.
- `setup.ps1`: installs it from a Windows PC over one SSH connection per step
  (`install`, `start`, `update`, `status`) and saves the app's keys to
  `%APPDATA%\Chatterino2\Misc\ModActivity\remote.json`.

The app (`src/util/ModActivity.cpp`) reads `remote.json`, pulls new actions
when the Mod activity window opens and every 3 minutes while it is open, and
merges them into the local files. An action the app saw itself within 5
seconds of the server's copy counts once.

Endpoints, all `GET`, all behind Cloudflare Access and the API key:

- `/v1/status`: `{channel, signedIn, needsSignIn, connected, lastEventAt, serverTime}`
- `/v1/<channel>/actions?since=<msecs>`: action lines newer than `since`
- `/v1/<channel>/streams`: `streams.tsv`

To stop counting: `docker compose down` in `~/apps/modtracker`, then revoke
the app at twitch.tv/settings/connections.
