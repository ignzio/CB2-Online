# Publishes the website (site\) to the PUBLIC repository that carries the releases, so the source repository can stay private.
#   powershell -File tools\publish_site.ps1 [-Repo owner/name] [-Message "text"]
# It clones the public repository, replaces its site files (index.html, style.css, app.js, config.js, img, .nojekyll and the Pages workflow), commits and pushes.
# GitHub Pages then serves it: in that repository open Settings > Pages and choose "GitHub Actions" as the source (once).
param(
    [string]$Repo = "ignzio/CB2-Online",
    [string]$Message = "Update the website"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$site = Join-Path $root "site"
if (-not (Test-Path (Join-Path $site "index.html"))) { throw "site\index.html not found" }
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("cb2-site-" + [guid]::NewGuid().ToString("N"))
git clone --depth 1 "https://github.com/$Repo.git" $tmp 2>&1 | Out-Host
if (-not (Test-Path (Join-Path $tmp ".git"))) { throw "could not clone $Repo (does it exist, and are you logged in to GitHub?)" }
try {
    foreach ($name in "index.html", "style.css", "app.js", "config.js", ".nojekyll") { Copy-Item (Join-Path $site $name) $tmp -Force }
    if (Test-Path (Join-Path $tmp "img")) { Remove-Item (Join-Path $tmp "img") -Recurse -Force }
    Copy-Item (Join-Path $site "img") (Join-Path $tmp "img") -Recurse
    $wf = Join-Path $tmp ".github\workflows"
    New-Item -ItemType Directory -Force $wf | Out-Null
    Copy-Item (Join-Path $site "pages.yml") (Join-Path $wf "pages.yml") -Force
    Push-Location $tmp
    git add -A
    if (git status --porcelain) {
        git commit -q -m $Message -m "Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>"
        git push origin HEAD
        "Website pushed to $Repo. Pages: https://$($Repo.Split('/')[0].ToLower()).github.io/$($Repo.Split('/')[1])/"
    } else { "Nothing changed." }
    Pop-Location
} finally {
    Set-Location $root
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
