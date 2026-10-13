# Chatterino9 mod tracker: sets up the service on your server from this PC.
#
#   .\setup.ps1 install   copies the service to ~/apps/modtracker and builds
#                         it. Nothing is started.
#   .\setup.ps1 start     fresh start: clears papaplatte's counts on the
#                         server and on this PC, then starts counting (and
#                         shows the Twitch sign-in code if it needs one).
#   .\setup.ps1 update    copies new service files and rebuilds and restarts
#                         the counter. Keeps its sign-in and its data.
#   .\setup.ps1 signin    restarts the counter so it shows a new Twitch
#                         sign-in code. Keeps its data.
#   .\setup.ps1 stop      pauses counting and clears what the server counted.
#                         Keeps the Twitch sign-in; start resumes.
#   .\setup.ps1 status    shows whether it runs and its last log lines.
#   .\setup.ps1 check     tests mods.acri.fyi from this PC like Chatterino9
#                         does. Does not connect to the server over SSH.
#
# Each step makes exactly one SSH connection with your key. It never asks
# for a password and never retries a failed login, so it can't trip the
# server's login ban. Secrets are sent through that connection, never put on
# a command line, and are not written to this chat or to the repository.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('install', 'start', 'update', 'signin', 'stop', 'status', 'check')]
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
        Write-Host 'It runs and counts mod actions in papaplatte.' -ForegroundColor Green
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
            Where-Object { $_.Name -like 'actions-*.tsv' -or $_.Name -eq 'streams.tsv' -or $_.Name -eq 'server-sync.json' } |
            Remove-Item -Force
        Write-Host 'Cleared the counts on this PC.'
    }
    $script = @"
set -eu
cd `$HOME/apps/modtracker
docker compose stop modtracker
n=`$(ls data/*/actions-*.tsv data/*/streams.tsv 2>/dev/null | wc -l)
rm -f data/*/actions-*.tsv data/*/streams.tsv
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
since=`$(date -u -d '-1 sec' +%Y-%m-%dT%H:%M:%SZ)
docker compose up -d
echo "Updated. Waiting for the counter..."
"@ + "`n" + $waitForCounter
    $code = Invoke-Remote $target $script
    if ($code -eq 0) { Resume-AppConfig }
    Show-StartResult $code
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
        Write-Host "3. All keys: channel $($s.channel), signed in $($s.signedIn), connected to Twitch $($s.connected)" -ForegroundColor Green
        if (-not $s.signedIn -or -not $s.connected) { $failed = $true }
    } else {
        Write-Host "3. All keys: expected 200, got $($r.code)" -ForegroundColor Red; $failed = $true
    }

    if ($failed) { Stop-Setup 'Something is off. Send these lines to Claude.' }
    Write-Host ''
    Write-Host 'Everything works the way Chatterino9 will use it.' -ForegroundColor Green
    exit 0
}
