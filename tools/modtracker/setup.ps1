# Chatterino9 mod tracker: sets up the service on your server from this PC.
#
#   .\setup.ps1 install   copies the service to ~/apps/modtracker and builds
#                         it. Nothing is started.
#   .\setup.ps1 start     fresh start: clears papaplatte's mod action counts
#                         on the server and on this PC, then starts counting
#                         (and shows the Twitch sign-in code if it needs one).
#                         Emote stats are kept.
#   .\setup.ps1 update    copies new service files and rebuilds and restarts
#                         the counter. Keeps its sign-in and its data.
#   .\setup.ps1 tunnel    moves the Cloudflare tunnel container to the
#                         cloudflared version in compose.yaml here. Only the
#                         tunnel restarts; the counter, its data, sign-in and
#                         secrets stay as they are.
#   .\setup.ps1 signin    restarts the counter so it shows a new Twitch
#                         sign-in code. Keeps its data.
#   .\setup.ps1 stop      pauses counting and clears the mod actions the
#                         server counted (emote stats are kept). Keeps the
#                         Twitch sign-in; start resumes.
#   .\setup.ps1 status    shows whether it runs and its last log lines.
#   .\setup.ps1 check     tests mods.acri.fyi from this PC like Chatterino9
#                         does. Does not connect to the server over SSH.
#   .\setup.ps1 friend-add     gives a friend a read-only key (emote stats,
#                         and mod activity if you say so) and makes a zip for
#                         them with your settings, tabs and stats, without
#                         your Twitch login. Their key can't change or delete
#                         anything.
#   .\setup.ps1 friend-remove  takes a friend's key away again.
#   .\setup.ps1 friends   lists the friends that have a key.
#
# Each step makes exactly one SSH connection with your key. It never asks
# for a password and never retries a failed login, so it can't trip the
# server's login ban. Secrets are sent through that connection, never put on
# a command line, and are not written to this chat or to the repository.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('install', 'start', 'update', 'tunnel', 'signin', 'stop', 'status', 'check', 'friend-add', 'friend-remove', 'friends')]
    [string]$Step
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$remembered = Join-Path $here 'ssh-target.txt'
$remoteDir = '~/apps/modtracker'
$publicUrl = 'https://mods.acri.fyi'

function Stop-Setup([string]$message) {
    Write-Host ''
    Write-Host "STOPPED: $message" -ForegroundColor Red
    exit 1
}

function Read-Secret([string]$prompt) {
    $secure = Read-Host -Prompt $prompt -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try {
        return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr).Trim()
    } finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
    }
}

function Get-SshTarget {
    $default = ''
    if (Test-Path $remembered) { $default = (Get-Content $remembered -Raw).Trim() }
    $prompt = 'SSH target you normally use for the server (for example ubuntu@1.2.3.4 or your ssh alias)'
    if ($default) { $prompt += " [$default]" }
    $target = (Read-Host -Prompt $prompt).Trim()
    if (-not $target) { $target = $default }
    if ($target -notmatch '^[A-Za-z0-9._@:\-]+$') { Stop-Setup 'That does not look like an SSH target.' }
    Set-Content -Path $remembered -Value $target -Encoding ASCII
    return $target
}

# Runs a bash script on the server over one SSH connection. The script goes
# through stdin, so secrets in it never show up in a process list.
function Invoke-Remote([string]$target, [string]$script) {
    if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
        Stop-Setup 'ssh was not found on this PC.'
    }
    $script = $script -replace "`r", ''
    $script | & ssh -o BatchMode=yes -o ConnectTimeout=15 -T $target "tr -d '\r' | bash -s" | Out-Host
    return $LASTEXITCODE
}

function Confirm-Step([string]$what) {
    Write-Host ''
    Write-Host $what -ForegroundColor Yellow
    $answer = Read-Host -Prompt 'Type yes to go ahead'
    if ($answer -ne 'yes') { Stop-Setup 'Nothing was changed.' }
}

$serviceFiles = @('server.mjs', 'Dockerfile', 'compose.yaml')

# The service files packed as base64 lines for a heredoc
function New-Bundle {
    foreach ($f in $serviceFiles) {
        if (-not (Test-Path (Join-Path $here $f))) { Stop-Setup "$f is missing next to this script." }
    }
    $bundle = Join-Path $env:TEMP 'modtracker-bundle.tgz'
    if (Test-Path $bundle) { Remove-Item $bundle }
    & tar -czf $bundle -C $here @serviceFiles
    if ($LASTEXITCODE -ne 0) { Stop-Setup 'Could not pack the service files.' }
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($bundle)) -replace '(.{76})', "`$1`n"
    Remove-Item $bundle
    return $b64
}

# Shows the counter's log since $since until it counts, gives up when it
# keeps crashing
$waitForCounter = @'
seen=0
for i in $(seq 1 360); do
  out=$(docker compose logs --since "$since" --no-log-prefix modtracker 2>&1)
  n=$(printf '%s\n' "$out" | wc -l)
  printf '%s\n' "$out" | tail -n +$((seen + 1))
  seen=$n
  case "$out" in *"Counting mod actions"*) echo "DONE"; exit 0 ;; esac
  restarts=$(docker inspect -f '{{.RestartCount}}' modtracker 2>/dev/null || echo 0)
  if [ "$restarts" -ge 3 ]; then
    docker compose stop modtracker
    echo "STOP: the counter crashed 3 times, so it was stopped. The tunnel still runs."
    exit 20
  fi
  sleep 5
done
echo "Still not signed in after 30 minutes. Run .\setup.ps1 status to see the code again."
exit 21
'@

function Show-StartResult([int]$code) {
    if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying.' }
    if ($code -eq 0) {
        Write-Host ''
        Write-Host 'It runs and counts mod actions and emotes in papaplatte.' -ForegroundColor Green
        exit 0
    }
    if ($code -eq 20) { Stop-Setup 'The counter kept crashing and was stopped. Send the lines above to Claude.' }
    if ($code -eq 21) { Stop-Setup 'No Twitch sign-in yet. The counter keeps waiting; .\setup.ps1 status shows the code.' }
    Stop-Setup "The server stopped this step (code $code)."
}

# While paused, Chatterino9 finds no remote.json and leaves the server out
$appConfig = Join-Path $env:APPDATA 'Chatterino2\Misc\ModActivity\remote.json'
$pausedConfig = "$appConfig.paused"

function Resume-AppConfig {
    if ((Test-Path $pausedConfig) -and -not (Test-Path $appConfig)) {
        Rename-Item -Path $pausedConfig -NewName (Split-Path -Leaf $appConfig)
    }
}

if ($Step -eq 'install') {
    $target = Get-SshTarget
    $clientId = (Read-Host -Prompt 'Twitch app Client ID').Trim()
    if ($clientId -notmatch '^[a-z0-9]{20,40}$') { Stop-Setup 'That does not look like a Twitch Client ID.' }
    # Pasted values may still carry the labels Cloudflare shows around them
    $tunnelToken = (Read-Secret 'Cloudflare tunnel token (starts with eyJ, input is hidden)') -replace '^.*\binstall\s+', ''
    if ($tunnelToken -notmatch '^eyJ[A-Za-z0-9_\-=+/.]+$') { Stop-Setup 'That does not look like a tunnel token. Paste only the part starting with eyJ.' }
    $accessId = ((Read-Host -Prompt 'Cloudflare Access service token Client ID (ends with .access)').Trim()) -replace '^CF-Access-Client-Id:\s*', ''
    if ($accessId -notmatch '^[A-Za-z0-9]+\.access$') { Stop-Setup 'That does not look like an Access Client ID.' }
    $accessSecret = (Read-Secret 'Cloudflare Access service token Client Secret (input is hidden)') -replace '^CF-Access-Client-Secret:\s*', ''
    if ($accessSecret -notmatch '^[A-Za-z0-9_\-+/=.]{20,256}$') {
        Stop-Setup "That does not look like an Access Client Secret ($($accessSecret.Length) characters, it should be one word with no spaces). Paste only the secret value."
    }

    # The key only Chatterino9 and the service know
    $bytes = New-Object byte[] 32
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $apiKey = -join ($bytes | ForEach-Object { $_.ToString('x2') })

    $b64 = New-Bundle

    Confirm-Step @"
This connects once to $target and:
  - stops without changing anything if ~/apps/modtracker, a modtracker
    container or a modtracker network already exists, or Docker is unusable
  - creates ~/apps/modtracker with the service files and its secrets
  - builds the modtracker image and downloads cloudflared
It does not start anything and does not touch any other app.
"@

    $script = @"
set -eu
APP=`$HOME/apps/modtracker
[ ! -e "`$APP" ] || { echo "STOP: `$APP already exists"; exit 10; }
docker info >/dev/null 2>&1 || { echo "STOP: Docker can't be used as `$(whoami)"; exit 11; }
if docker ps -a --format '{{.Names}}' | grep -qx -e modtracker -e modtracker-tunnel; then
  echo "STOP: a modtracker container already exists"; exit 12
fi
if docker network inspect modtracker >/dev/null 2>&1; then
  echo "STOP: a modtracker network already exists"; exit 13
fi
umask 077
mkdir -p "`$APP/data"
base64 -d > "`$APP/bundle.tgz" <<'B64'
$b64
B64
tar -xzf "`$APP/bundle.tgz" -C "`$APP"
rm "`$APP/bundle.tgz"
chmod 644 "`$APP/server.mjs" "`$APP/Dockerfile" "`$APP/compose.yaml"
cat > "`$APP/app.env" <<'ENVEOF'
TWITCH_CLIENT_ID=$clientId
API_KEY=$apiKey
CHANNEL=papaplatte
ENVEOF
cat > "`$APP/tunnel.env" <<'ENVEOF'
TUNNEL_TOKEN=$tunnelToken
ENVEOF
printf 'HOST_UID=%s\nHOST_GID=%s\n' "`$(id -u)" "`$(id -g)" > "`$APP/.env"
cd "`$APP"
docker compose build --pull
docker compose pull tunnel
echo "BUILT: nothing is running yet"
"@

    $code = Invoke-Remote $target $script
    if ($code -ne 0) {
        if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying. Check the SSH target and that your key works with a normal ssh login.' }
        Stop-Setup "The server stopped the setup (code $code). Nothing was started."
    }

    # Where Chatterino9 finds the server and its keys
    $dir = Join-Path $env:APPDATA 'Chatterino2\Misc\ModActivity'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $config = [ordered]@{
        url                = $publicUrl
        apiKey             = $apiKey
        accessClientId     = $accessId
        accessClientSecret = $accessSecret
    }
    $json = $config | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $dir 'remote.json'), $json, (New-Object Text.UTF8Encoding $false))

    Write-Host ''
    Write-Host 'Installed and built. Nothing is running yet.' -ForegroundColor Green
    Write-Host "Chatterino9's key was saved to $dir\remote.json"
    Write-Host 'Next: .\setup.ps1 start'
    exit 0
}

if ($Step -eq 'start') {
    # Chatterino9 keeps the counts in memory and would write them back
    if (Get-Process -Name chatterino -ErrorAction SilentlyContinue) {
        Stop-Setup 'Close Chatterino9 first (also from the tray), then run start again.'
    }
    $localDir = Join-Path $env:APPDATA 'Chatterino2\Misc\ModActivity\papaplatte'
    $target = Get-SshTarget
    Confirm-Step @"
Fresh start. This deletes every papaplatte mod action counted so far:
  - on this PC: actions and streams in $localDir
  - on the server: everything the counter saved
Then it connects once to $target, starts the two modtracker containers
(the counter and the Cloudflare tunnel) and counting begins from now.
If the counter needs a Twitch sign-in, it shows the code; approve it at
https://www.twitch.tv/activate with your mod account. If the counter
crashes 3 times, it stops the counter again.
"@
    if (Test-Path $localDir) {
        Get-ChildItem -Path $localDir -File |
            Where-Object { $_.Name -like 'actions-*.tsv' -or $_.Name -eq 'streams.tsv' -or $_.Name -eq 'server-sync.json' -or $_.Name -eq 'tracking.json' } |
            Remove-Item -Force
        Write-Host 'Cleared the counts on this PC.'
    }
    $script = @"
set -eu
cd `$HOME/apps/modtracker
docker compose stop modtracker
n=`$(ls data/*/actions-*.tsv data/*/streams.tsv 2>/dev/null | wc -l)
rm -f data/*/actions-*.tsv data/*/streams.tsv data/*/tracking.json
echo "Cleared the counts on the server (`$n files)."
since=`$(date -u -d '-1 sec' +%Y-%m-%dT%H:%M:%SZ)
docker compose up -d
echo "Started. Counting from now on..."
"@ + "`n" + $waitForCounter
    $code = Invoke-Remote $target $script
    if ($code -eq 0) { Resume-AppConfig }
    Show-StartResult $code
}

if ($Step -eq 'update') {
    $target = Get-SshTarget
    $b64 = New-Bundle
    Confirm-Step @"
This connects once to $target and, in ~/apps/modtracker only:
  - replaces server.mjs, Dockerfile and compose.yaml with the ones here
  - rebuilds the counter and restarts it (the tunnel keeps running)
Its secrets, its Twitch sign-in and its data stay as they are. If it has
no sign-in yet, it shows the Twitch code like start does. The counter also
counts emotes in chat; it reads chat as an anonymous viewer, no login.
"@
    $script = @"
set -eu
APP=`$HOME/apps/modtracker
[ -f "`$APP/app.env" ] || { echo "STOP: `$APP is not set up, run install first"; exit 10; }
cd "`$APP"
base64 -d > bundle.tgz <<'B64'
$b64
B64
tar -xzf bundle.tgz server.mjs Dockerfile compose.yaml
rm bundle.tgz
chmod 644 server.mjs Dockerfile compose.yaml
docker compose build --pull modtracker
since=`$(date -u -d '-1 sec' +%Y-%m-%dT%H:%M:%SZ)
docker compose up -d
echo "Updated. Waiting for the counter..."
"@ + "`n" + $waitForCounter
    $code = Invoke-Remote $target $script
    if ($code -eq 0) { Resume-AppConfig }
    Show-StartResult $code
}

if ($Step -eq 'tunnel') {
    $target = Get-SshTarget
    $compose = Join-Path $here 'compose.yaml'
    if (-not (Test-Path $compose)) { Stop-Setup 'compose.yaml is missing next to this script.' }
    $image = Select-String -Path $compose -Pattern 'cloudflare/cloudflared:[0-9.]+' | Select-Object -First 1
    if (-not $image) { Stop-Setup 'compose.yaml here names no cloudflared version.' }
    $image = $image.Matches[0].Value
    $b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($compose)) -replace '(.{76})', "`$1`n"
    Confirm-Step @"
This connects once to $target and, in ~/apps/modtracker only:
  - stops without changing anything if the compose.yaml there differs from
    the one here in more than the cloudflared version
  - downloads $image
  - restarts only the tunnel container with it (mods.acri.fyi is gone for
    a few seconds) and waits until it is connected to Cloudflare again
  - goes back to the old version by itself if it does not connect
The counter keeps running. Its counts, Twitch sign-in, tunnel token and
keys stay as they are.
"@
    $script = @"
set -eu
APP=`$HOME/apps/modtracker
[ -f "`$APP/tunnel.env" ] || { echo "STOP: `$APP is not set up, run install first"; exit 10; }
cd "`$APP"
base64 -d > compose.yaml.new <<'B64'
$b64
B64
tr -d '\r' < compose.yaml.new > compose.yaml.tmp && mv compose.yaml.tmp compose.yaml.new
if ! diff <(grep -v 'cloudflare/cloudflared:' compose.yaml) <(grep -v 'cloudflare/cloudflared:' compose.yaml.new) > /dev/null; then
  rm compose.yaml.new
  echo "STOP: compose.yaml on the server differs in more than the cloudflared version. Nothing was changed."
  exit 11
fi
echo "Running now: `$(docker inspect -f '{{.Config.Image}}' modtracker-tunnel)"
docker pull $image
cp -p compose.yaml compose.yaml.before-tunnel-update
mv compose.yaml.new compose.yaml
chmod 644 compose.yaml
since=`$(date -u -d '-1 sec' +%Y-%m-%dT%H:%M:%SZ)
docker compose up -d --no-deps tunnel || true
for i in `$(seq 1 18); do
  sleep 5
  out=`$(docker compose logs --since "`$since" --no-log-prefix tunnel 2>&1)
  case "`$out" in *"Registered tunnel connection"*)
    printf '%s\n' "`$out" | grep -m1 -E 'Version|version=' || true
    echo "Running now: `$(docker inspect -f '{{.Config.Image}}' modtracker-tunnel)"
    echo "DONE"
    exit 0 ;;
  esac
done
printf '%s\n' "`$out" | tail -n 20
echo "The new tunnel did not connect within 90 seconds, going back to the old version."
mv compose.yaml.before-tunnel-update compose.yaml
docker compose up -d --no-deps tunnel || true
exit 22
"@
    $code = Invoke-Remote $target $script
    if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying.' }
    if ($code -eq 11) { Stop-Setup 'compose.yaml here does not match the server. Send these lines to Claude.' }
    if ($code -eq 22) { Stop-Setup 'The new tunnel did not connect, the old version runs again. Send the lines above to Claude.' }
    if ($code -ne 0) { Stop-Setup "The server stopped this step (code $code)." }
    Write-Host ''
    Write-Host "The tunnel runs $image and is connected." -ForegroundColor Green
    exit 0
}

if ($Step -eq 'signin') {
    $target = Get-SshTarget
    Confirm-Step @"
This connects once to $target and restarts the counter in
~/apps/modtracker. If it needs a Twitch sign-in, it shows the code; approve
it at https://www.twitch.tv/activate with your mod account. Its counts stay.
"@
    $script = @"
set -eu
cd `$HOME/apps/modtracker
since=`$(date -u -d '-1 sec' +%Y-%m-%dT%H:%M:%SZ)
docker compose restart modtracker
echo "Restarted. Waiting for the counter..."
"@ + "`n" + $waitForCounter
    $code = Invoke-Remote $target $script
    if ($code -eq 0) { Resume-AppConfig }
    Show-StartResult $code
}

if ($Step -eq 'stop') {
    $target = Get-SshTarget
    Confirm-Step @"
This connects once to $target and, in ~/apps/modtracker only:
  - stops the counter and the Cloudflare tunnel
  - deletes the mod actions and streams the server counted so far
Its Twitch sign-in stays, so .\setup.ps1 start resumes without a new code.
Chatterino9 leaves the server out until then.
"@
    $script = @"
set -eu
cd `$HOME/apps/modtracker
docker compose stop
rm -f data/*/actions-*.tsv data/*/streams.tsv
echo "PAUSED: nothing is counted until start"
"@
    $code = Invoke-Remote $target $script
    if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying.' }
    if ($code -ne 0) { Stop-Setup "The server stopped this step (code $code)." }
    if (Test-Path $appConfig) { Move-Item -Force -Path $appConfig -Destination $pausedConfig }
    Write-Host ''
    Write-Host 'Paused. Run .\setup.ps1 start when counting should begin.' -ForegroundColor Green
    exit 0
}

if ($Step -eq 'status') {
    $target = Get-SshTarget
    $script = @"
cd `$HOME/apps/modtracker && docker compose ps && docker compose logs --tail 15 --no-log-prefix modtracker
"@
    $code = Invoke-Remote $target $script
    if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying.' }
    exit 0
}

# ---------------------------------------------------------------- friends ---
# A friend's key is checked by the counter against viewers.json in its data
# folder, which holds only the key's SHA-256, never the key itself.

$channel = 'papaplatte'

function Read-FriendName {
    $name = (Read-Host -Prompt "Your friend's name, used only to tell keys apart (letters, digits, - or _)").Trim().ToLower()
    if ($name -notmatch '^[a-z0-9_-]{1,32}$') { Stop-Setup 'Use 1 to 32 letters, digits, - or _.' }
    return $name
}

# settings.json without your Twitch and Kick logins (Chatterino keeps their
# tokens there), upload keys and paths on this PC
$dropSections = @('accounts', 'kickAccounts', 'external')
function Test-DropSection([string]$key) {
    return ($dropSections -contains $key) -or ($key -match 'account|token|secret|password|credential')
}

function Get-FriendSettings([string]$path) {
    $text = [IO.File]::ReadAllText($path)
    if ($PSVersionTable.PSVersion.Major -ge 6) {
        # keeps every value exactly as it is
        Add-Type -AssemblyName System.Text.Json
        $root = [Text.Json.Nodes.JsonNode]::Parse($text).AsObject()
        foreach ($key in @($root | ForEach-Object { $_.Key })) {
            if (Test-DropSection $key) { [void]$root.Remove($key) }
        }
        foreach ($section in @('logging', 'cache')) {
            if ($root.ContainsKey($section) -and $root[$section] -is [Text.Json.Nodes.JsonObject]) {
                [void]$root[$section].Remove('path')
            }
        }
        $out = $root.ToJsonString()
    } else {
        Add-Type -AssemblyName System.Web.Extensions
        $json = New-Object Web.Script.Serialization.JavaScriptSerializer
        $json.MaxJsonLength = [int]::MaxValue
        $json.RecursionLimit = 1000
        $root = $json.DeserializeObject($text)
        if ($root -isnot [Collections.Generic.IDictionary[string, object]]) { Stop-Setup 'settings.json could not be read.' }
        foreach ($key in @($root.Keys)) {
            if (Test-DropSection $key) { [void]$root.Remove($key) }
        }
        foreach ($section in @('logging', 'cache')) {
            if ($root.ContainsKey($section) -and $root[$section] -is [Collections.Generic.IDictionary[string, object]]) {
                [void]$root[$section].Remove('path')
            }
        }
        $out = $json.Serialize($root)
    }
    # last check on the finished text: no login token may be left anywhere
    if ($out -match '"(oauthToken|authToken|refreshToken|clientSecret|accounts|kickAccounts)"\s*:') {
        Stop-Setup "Could not take every login out of settings.json ($($Matches[1]) is still there), nothing was made."
    }
    return $out
}

if ($Step -eq 'friend-add') {
    $appDir = Join-Path $env:APPDATA 'Chatterino2'
    if (-not (Test-Path $appConfig)) { Stop-Setup 'remote.json is missing on this PC, so there is no server to share.' }
    $own = Get-Content $appConfig -Raw | ConvertFrom-Json
    if ($own.viewer) { Stop-Setup "This PC has a friend's key, run friend-add on the PC that set up the server." }
    $settingsFile = Join-Path $appDir 'Settings\settings.json'
    $layoutFile = Join-Path $appDir 'Settings\window-layout.json'
    if (-not (Test-Path $settingsFile) -or -not (Test-Path $layoutFile)) { Stop-Setup "Chatterino9's settings were not found in $appDir\Settings." }

    $name = Read-FriendName
    $answer = (Read-Host -Prompt 'Share mod activity too? Type yes for emote stats and mod activity, no for emote stats only').Trim().ToLower()
    if ($answer -eq 'yes') { $scope = 'all' } elseif ($answer -eq 'no') { $scope = 'emotes' } else { Stop-Setup 'Type yes or no.' }

    Write-Host ''
    Write-Host 'Your friend needs their own Cloudflare service token, so you can take it away without touching yours:'
    Write-Host '  1. Cloudflare Zero Trust > Access > Service auth > Create service token, name it'
    Write-Host "     chatterino9-$name, duration Non-expiring. Keep the page open, the secret shows only once."
    Write-Host '  2. Access > Applications > the mods.acri.fyi app > Policies > edit the policy that'
    Write-Host "     includes the chatterino9 token > add chatterino9-$name to the same Include rule > Save."
    $accessId = ((Read-Host -Prompt "Client ID of chatterino9-$name (ends with .access)").Trim()) -replace '^CF-Access-Client-Id:\s*', ''
    if ($accessId -notmatch '^[A-Za-z0-9]+\.access$') { Stop-Setup 'That does not look like an Access Client ID.' }
    if ($accessId -eq $own.accessClientId) { Stop-Setup "That is your own token. Make a new one for $name, so you can take theirs away without breaking yours." }
    $accessSecret = (Read-Secret "Client Secret of chatterino9-$name (input is hidden)") -replace '^CF-Access-Client-Secret:\s*', ''
    if ($accessSecret -notmatch '^[A-Za-z0-9_\-+/=.]{20,256}$') {
        Stop-Setup "That does not look like an Access Client Secret ($($accessSecret.Length) characters). Paste only the secret value."
    }

    # The friend's own key; the server only gets its hash
    $bytes = New-Object byte[] 32
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $friendKey = -join ($bytes | ForEach-Object { $_.ToString('x2') })
    $sha = [Security.Cryptography.SHA256]::Create()
    $hash = -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($friendKey)) | ForEach-Object { $_.ToString('x2') })
    $what = if ($scope -eq 'all') { 'emote stats and mod activity' } else { 'emote stats only' }
    $zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "Chatterino9-for-$name.zip"

    if (Get-Process -Name chatterino -ErrorAction SilentlyContinue) {
        Write-Host ''
        Write-Host 'Chatterino9 is open: the zip gets your settings as Chatterino9 last saved them. Close it first for the newest.' -ForegroundColor Yellow
    }
    $target = Get-SshTarget
    Confirm-Step @"
This connects once to $target and adds a read-only key for $name to
~/apps/modtracker/data/viewers.json ($what). The counter picks it up
without a restart; nothing else on the server changes. A key named $name
that is already there is replaced.
Then it makes $zip with:
  - your settings, tabs, splits and highlights, without your Twitch login,
    upload keys or folder paths
  - your emote stats$(if ($scope -eq 'all') { ' and papaplatte mod activity counts' }) so far
  - $name's key and Cloudflare token
Send the zip to $name privately: whoever has it can read your stats.
"@
    $script = @"
set -eu
APP=`$HOME/apps/modtracker
[ -f "`$APP/app.env" ] || { echo "STOP: `$APP is not set up"; exit 10; }
grep -q 'viewers.json' "`$APP/server.mjs" || { echo "STOP: the counter is too old for friend keys, run .\setup.ps1 update first"; exit 14; }
python3 - <<'PY'
import json, os
path = os.path.expanduser('~/apps/modtracker/data/viewers.json')
try:
    with open(path) as f:
        viewers = json.load(f)
except FileNotFoundError:
    viewers = {}
viewers['$name'] = {'hash': '$hash', 'scope': '$scope'}
tmp = path + '.tmp'
with open(tmp, 'w') as f:
    json.dump(viewers, f, indent=2)
os.chmod(tmp, 0o600)
os.replace(tmp, path)
print('Friend keys now: ' + ', '.join(sorted(viewers)))
PY
"@
    $code = Invoke-Remote $target $script
    if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying.' }
    if ($code -eq 14) { Stop-Setup 'Run .\setup.ps1 update first (with the new server files), then friend-add again.' }
    if ($code -ne 0) { Stop-Setup "The server stopped this step (code $code). No key was added." }

    $stage = Join-Path $env:TEMP "chatterino9-friend-$name"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    $out = Join-Path $stage 'Chatterino2'
    $utf8 = New-Object Text.UTF8Encoding $false
    try {
        New-Item -ItemType Directory -Force -Path (Join-Path $out 'Settings') | Out-Null
        [IO.File]::WriteAllText((Join-Path $out 'Settings\settings.json'), (Get-FriendSettings $settingsFile), $utf8)
        Copy-Item $layoutFile (Join-Path $out 'Settings\window-layout.json')

        $stats = Join-Path $appDir 'Misc\EmoteStats'
        if (Test-Path $stats) {
            New-Item -ItemType Directory -Force -Path (Join-Path $out 'Misc') | Out-Null
            Copy-Item -Recurse $stats (Join-Path $out 'Misc\EmoteStats')
        }
        $friendMods = Join-Path $out 'Misc\ModActivity'
        New-Item -ItemType Directory -Force -Path $friendMods | Out-Null
        if ($scope -eq 'all') {
            $mods = Join-Path $appDir "Misc\ModActivity\$channel"
            if (Test-Path $mods) {
                New-Item -ItemType Directory -Force -Path (Join-Path $friendMods $channel) | Out-Null
                Get-ChildItem -Path $mods -File |
                    Where-Object { $_.Name -like 'actions-*.tsv' -or $_.Name -eq 'streams.tsv' -or $_.Name -eq 'server-sync.json' } |
                    Copy-Item -Destination (Join-Path $friendMods $channel)
            }
        }
        $config = [ordered]@{
            url                = $publicUrl
            apiKey             = $friendKey
            accessClientId     = $accessId
            accessClientSecret = $accessSecret
            viewer             = $scope
            channel            = $channel
        }
        [IO.File]::WriteAllText((Join-Path $friendMods 'remote.json'), ($config | ConvertTo-Json), $utf8)

        $readme = @"
Chatterino9 from Phil

1. Download Chatterino9-windows-x86-64.zip from
   https://github.com/AcriVFX/chatterino7/releases/latest
   and unzip it to a folder of your choice, for example
   %LOCALAPPDATA%\Programs\Chatterino9. Don't start it yet.
2. If you already use Chatterino or Chatterino7: close it, then back up the
   folder %APPDATA%\Chatterino2 (this pack replaces its settings).
3. Press Win+R, type %APPDATA% and press Enter. Copy the Chatterino2 folder
   from this zip into that folder and let it replace the files.
4. Start chatterino.exe and log in with your own Twitch account.

Emote stats$(if ($scope -eq 'all') { ' and Mod activity' }) show Phil's numbers and update live. They are
read-only for you. Keep this zip to yourself: it holds your personal key.
"@
        [IO.File]::WriteAllText((Join-Path $stage 'README.txt'), ($readme -replace "`r?`n", "`r`n"), $utf8)

        if (Test-Path $zip) { Remove-Item -Force $zip }
        Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    } finally {
        if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    }

    Write-Host ''
    Write-Host "Done. $name's key works now ($what)." -ForegroundColor Green
    Write-Host "Send $zip to $name privately. The README inside says what to do."
    Write-Host "To take it away again: .\setup.ps1 friend-remove, and delete chatterino9-$name in Cloudflare."
    exit 0
}

if ($Step -eq 'friend-remove') {
    $name = Read-FriendName
    $target = Get-SshTarget
    Confirm-Step @"
This connects once to $target and removes $name's key from
~/apps/modtracker/data/viewers.json. From then on their app gets no new
numbers (what it already downloaded stays on their PC). Also delete the
service token chatterino9-$name in Cloudflare Zero Trust > Access >
Service auth.
"@
    $script = @"
set -eu
python3 - <<'PY'
import json, os, sys
path = os.path.expanduser('~/apps/modtracker/data/viewers.json')
try:
    with open(path) as f:
        viewers = json.load(f)
except FileNotFoundError:
    viewers = {}
if viewers.pop('$name', None) is None:
    print('No key named $name.')
    sys.exit(15)
tmp = path + '.tmp'
with open(tmp, 'w') as f:
    json.dump(viewers, f, indent=2)
os.chmod(tmp, 0o600)
os.replace(tmp, path)
print('Friend keys now: ' + (', '.join(sorted(viewers)) or 'none'))
PY
"@
    $code = Invoke-Remote $target $script
    if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying.' }
    if ($code -eq 15) { Stop-Setup "There is no key named $name; .\setup.ps1 friends lists them." }
    if ($code -ne 0) { Stop-Setup "The server stopped this step (code $code)." }
    Write-Host ''
    Write-Host "$name's key no longer works." -ForegroundColor Green
    exit 0
}

if ($Step -eq 'friends') {
    $target = Get-SshTarget
    $script = @"
python3 - <<'PY'
import json, os
path = os.path.expanduser('~/apps/modtracker/data/viewers.json')
try:
    with open(path) as f:
        viewers = json.load(f)
except FileNotFoundError:
    viewers = {}
for name, entry in sorted(viewers.items()):
    print(name + ': ' + ('emote stats and mod activity' if entry.get('scope') == 'all' else 'emote stats only'))
if not viewers:
    print('No friend keys.')
PY
"@
    $code = Invoke-Remote $target $script
    if ($code -eq 255) { Stop-Setup 'SSH could not log in. Not retrying.' }
    exit 0
}

if ($Step -eq 'check') {
    $file = Join-Path $env:APPDATA 'Chatterino2\Misc\ModActivity\remote.json'
    if (-not (Test-Path $file)) { Stop-Setup 'remote.json is missing, run install first.' }
    $c = Get-Content $file -Raw | ConvertFrom-Json
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    function Get-Status([hashtable]$headers) {
        $req = [Net.HttpWebRequest]::Create("$($c.url)/v1/status")
        $req.AllowAutoRedirect = $false
        $req.Timeout = 20000
        foreach ($k in $headers.Keys) { $req.Headers.Add($k, $headers[$k]) }
        try {
            $resp = $req.GetResponse()
        } catch [Net.WebException] {
            $resp = $_.Exception.Response
            if (-not $resp) { return @{ code = 0; body = $_.Exception.Message } }
        }
        $code = [int]$resp.StatusCode
        $body = (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd()
        $resp.Close()
        return @{ code = $code; body = $body }
    }

    $access = @{ 'CF-Access-Client-Id' = $c.accessClientId; 'CF-Access-Client-Secret' = $c.accessClientSecret }
    $failed = $false

    $r = Get-Status @{}
    if ($r.code -eq 302 -or $r.code -eq 401 -or $r.code -eq 403) {
        Write-Host "1. Without keys: blocked by Cloudflare ($($r.code))" -ForegroundColor Green
    } else {
        Write-Host "1. Without keys: expected Cloudflare to block it, got $($r.code)" -ForegroundColor Red; $failed = $true
    }

    $r = Get-Status $access
    if ($r.code -eq 404) {
        Write-Host '2. Cloudflare keys only: reached the counter, which refused it without its own key' -ForegroundColor Green
    } else {
        Write-Host "2. Cloudflare keys only: expected 404 from the counter, got $($r.code)" -ForegroundColor Red; $failed = $true
    }

    $r = Get-Status ($access + @{ 'X-Modtracker-Key' = $c.apiKey })
    if ($r.code -eq 200) {
        $s = $r.body | ConvertFrom-Json
        Write-Host "3. All keys: channel $($s.channel), signed in $($s.signedIn), connected to Twitch $($s.connected), counting emotes $([bool]$s.emotes.connected)" -ForegroundColor Green
        if (-not $s.signedIn -or -not $s.connected -or -not $s.emotes.connected) { $failed = $true }
    } else {
        Write-Host "3. All keys: expected 200, got $($r.code)" -ForegroundColor Red; $failed = $true
    }

    if ($failed) { Stop-Setup 'Something is off. Send these lines to Claude.' }
    Write-Host ''
    Write-Host 'Everything works the way Chatterino9 will use it.' -ForegroundColor Green
    exit 0
}
