# Create a Gitea release and attach the packaged zip.
#
#   $env:GITEA_TOKEN = "<token>"
#   .\tools\publish-release.ps1 -Tag v0.1.0 -Zip dist\gakumas-mod-runtime-0.1.0.zip
#
# The release workflow calls this with the token Gitea injects.  Kept out of the
# YAML on purpose: quoting a JSON body and a multipart upload through YAML ->
# PowerShell -> curl is where this kind of step usually breaks.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [Parameter(Mandatory = $true)][string]$Zip,
    [string]$ApiBase = $env:GITHUB_API_URL,
    [string]$Repository = $env:GITHUB_REPOSITORY,
    [string]$Token = $env:GITEA_TOKEN,
    [switch]$Draft
)

$ErrorActionPreference = "Stop"

if (-not $Token) { throw "No token. Set GITEA_TOKEN or pass -Token." }
if (-not $ApiBase) { throw "No API base. Set GITHUB_API_URL or pass -ApiBase." }
if (-not $Repository) { throw "No repository. Set GITHUB_REPOSITORY or pass -Repository." }
if (-not (Test-Path $Zip)) { throw "No such file: $Zip" }

$api = "$($ApiBase.TrimEnd('/'))/repos/$Repository"
$name = Split-Path -Leaf $Zip
$zipHash = (Get-FileHash $Zip -Algorithm SHA256).Hash

# Single-quoted here-string: markdown backticks stay literal instead of being
# eaten by PowerShell's escape character.
$template = @'
解压到游戏根目录（`gakumas.exe` 所在处）：

```
<游戏目录>\xinput1_3.dll
<游戏目录>\gakumas-mod\config.json
```

已经装过的直接覆盖 `xinput1_3.dll`。**`gakumas-mod\config.json` 若已存在请勿覆盖**，
那是你自己的设置。

| 文件 | SHA-256 |
|---|---|
| `__NAME__` | `__ZIPHASH__` |

日志在 `<游戏目录>\gakumas-mod\`，每次启动的 `[BOOT]` 行会写出版本号 `__TAG__`；
报问题时请附上它。默认只记录错误，排查时把 `config.json` 的 `logLevel` 改成 `"info"`。
'@

$body = $template.
    Replace('__NAME__', $name).
    Replace('__ZIPHASH__', $zipHash).
    Replace('__TAG__', $Tag)

$payload = [ordered]@{
    tag_name   = $Tag
    name       = $Tag
    body       = $body
    draft      = [bool]$Draft
    prerelease = $Tag -match '-(rc|beta|alpha)'
} | ConvertTo-Json -Depth 3

$payloadFile = Join-Path ([System.IO.Path]::GetTempPath()) "gkms-release-$PID.json"
# -Encoding utf8 on Windows PowerShell 5.1 writes a BOM, which Gitea rejects.
[System.IO.File]::WriteAllText($payloadFile, $payload, [System.Text.UTF8Encoding]::new($false))

try {
    Write-Host "==> POST $api/releases ($Tag)"
    $created = & curl.exe -sS -f -X POST "$api/releases" `
        -H "Authorization: token $Token" `
        -H "Content-Type: application/json" `
        --data-binary "@$payloadFile"
    if ($LASTEXITCODE -ne 0) { throw "creating the release failed (curl $LASTEXITCODE)" }

    $id = ($created | ConvertFrom-Json).id
    if (-not $id) { throw "no release id in the response: $created" }

    Write-Host "==> upload $name to release $id"
    & curl.exe -sS -f -X POST "$api/releases/$id/assets?name=$name" `
        -H "Authorization: token $Token" `
        -F "attachment=@$Zip" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "uploading the asset failed (curl $LASTEXITCODE)" }

    Write-Host "published $Tag -> $api/releases"
}
finally {
    Remove-Item -Force $payloadFile -ErrorAction SilentlyContinue
}
