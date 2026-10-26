# The website and the public home of the project

The source repository can be **private**. What the world needs to see lives in a **public** repository (here: `ignzio/CB2-Online`, change it in `site/config.js`):

| What | Where |
|---|---|
| Downloads (the zip, `Crash2.exe`, `Crash2-Server.exe`) | GitHub **Releases** of the public repository |
| The page people read | `site/` -> GitHub **Pages** of the public repository |
| Bug reports | **Issues** of the public repository (Esc > Report a bug opens a prefilled one) |
| The in-game updater | reads the latest release of the public repository (no login is needed for a public repository) |

Why it matters: a release of a private repository cannot be downloaded without a login, and neither can the updater read it. If the repository where the releases are published is made
private, downloads and updates stop working for everybody else.

## One-time setup

1. Decide which repository is public. Easiest: keep `ignzio/CB2-Online` public and move the *source code* to a new private repository (for example `ignzio/CB2-Online-src`) - or the other way round,
   create a new public repository (for example `ignzio/CB2-Online-releases`) that has no source, only the website and the releases.
2. In the public repository: Settings > Pages > Source: **GitHub Actions**. Enable **Issues**.
3. Put the public repository's name in two places:
   - `site/config.js` (`repo`),
   - the game: `cmake -S . -B build -DCB2_RELEASE_REPO=owner/name` (the updater and the bug report button use it), and `tools\make_release.ps1 -Repo owner/name` when publishing.
4. `powershell -File tools\publish_site.ps1 -Repo owner/name` copies `site/` to the public repository and pushes it; the workflow in `site/pages.yml` publishes it.
5. In `site/config.js` fill in your donation links (an empty `url` hides the button) and keep `raised` up to date by hand; the milestone bars follow it.

## Editing the page

`site/index.html` (text), `site/style.css`, `site/config.js` (repository, donation links, milestones, amounts). Screenshots are in `site/img/`; replace them or add footage (a `<video>` in the hero
works well). Open `site/index.html` in a browser to preview it; the download section reads the latest release live from the GitHub API.

## Donations

The page only links to services you set up (Ko-fi, GitHub Sponsors, PayPal ...). It says that donations support the development of the open-source tools and unlock nothing. Keep it that way:
the game belongs to its rights holders, so do not sell access, perks inside the game or anything that contains game data, and do not use the game's name for products.
