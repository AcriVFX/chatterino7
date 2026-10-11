# Chatterino9 mod tracker: sets up the service on your server from this PC.
#
#   .\setup.ps1 install   copies the service to ~/apps/modtracker and builds
#                         it. Nothing is started.
#   .\setup.ps1 start     starts it and shows the Twitch sign-in code.
#   .\setup.ps1 update    copies new service files and rebuilds and restarts
#                         the counter. Keeps its sign-in and its data.
#   .\setup.ps1 status    shows whether it runs and its last log lines.
#
# Each step makes exactly one SSH connection with your key. It never asks
# for a password and never retries a failed login, so it can't trip the
# server's login ban. Secrets are sent through that connection, never put on
# a command line, and are not written to this chat or to the repository.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('install', 'start', 'update', 'status')]
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

# Shows the counter's log until it counts, gives up when it keeps crashing
$waitForCounter = @'
seen=0
for i in $(seq 1 360); do
  out=$(docker compose logs --no-log-prefix modtracker 2>&1)
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
        Write-Host 'It runs and counts mod actions in papaplatte.' -ForegroundColor Green
        exit 0
    }
    if ($code -eq 20) { Stop-Setup 'The counter kept crashing and was stopped. Send the lines above to Claude.' }
    if ($code -eq 21) { Stop-Setup 'No Twitch sign-in yet. The counter keeps waiting; .\setup.ps1 status shows the code.' }
    Stop-Setup "The server stopped this step (code $code)."
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
    $target = Get-SshTarget
    Confirm-Step @"
This connects once to $target and starts the two modtracker containers
(the counter and the Cloudflare tunnel). Then it shows the Twitch sign-in
code. Approve it at https://www.twitch.tv/activate with your mod account.
It waits up to 30 minutes for the sign-in. If the counter crashes 3 times,
it stops the counter again.
"@
    $script = @"
set -eu
cd `$HOME/apps/modtracker
docker compose up -d
echo "Started. Waiting for the sign-in code..."
"@ + "`n" + $waitForCounter
    Show-StartResult (Invoke-Remote $target $script)
}

if ($Step -eq 'update') {
    $target = Get-SshTarget
    $b64 = New-Bundle
    Confirm-Step @"
This connects once to $target and, in ~/apps/modtracker only:
  - replaces server.mjs, Dockerfile and compose.yaml with the ones here
  - rebuilds the counter and restarts it (the tunnel keeps running)
Its secrets, its Twitch sign-in and its data stay as they are. If it has
no sign-in yet, it shows the Twitch code like start does.
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
docker compose up -d
echo "Updated. Waiting for the counter..."
"@ + "`n" + $waitForCounter
    Show-StartResult (Invoke-Remote $target $script)
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
