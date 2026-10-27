# The website and the public home of the project

The page is served by **GitHub Pages** straight from this repository: **Settings > Pages > Source: Deploy from a branch > `main` / `/docs`**. GitHub Pages only offers the repository root (`/`) or `/docs`, so
the site lives in `docs/` next to the documentation:

| File | What |
|---|---|
| `docs/index.html` | the text of the page |
| `docs/style.css` | the look |
| `docs/app.js` | reads the latest release from the GitHub API and draws the milestone bars and donation buttons |
| `docs/config.js` | the repository, donation links, amount raised and the milestones |
| `docs/img/` | screenshots (shared with the README) |
| `docs/.nojekyll` | tells Pages not to run Jekyll, so the Markdown documents next to the page are served as they are |

Deploying is pushing to `main`: Pages rebuilds `docs/` within a minute or two. Nothing else to run.

Where everything lives:

| What | Where |
|---|---|
| Downloads (the zip, `Crash2.exe`, `Crash2-Server.exe`) | GitHub **Releases** of `ignzio/CB2-Online` |
| The page people read | `docs/` -> GitHub **Pages** (`https://ignzio.github.io/CB2-Online/`, or the account's custom domain if one is set) |
| Bug reports | **Issues** (Esc > Report a bug opens a prefilled one) |
| The in-game updater | reads the latest release (no login is needed while the repository is public) |

**The repository has to stay public** for all of this: a release of a private repository cannot be downloaded without a login, the updater cannot read it, and GitHub Pages for a private repository needs a
paid plan (and then serves the page publicly anyway, but the downloads would still need a login). If the source code must be private, keep a separate public repository for releases and the website
(`site/config.js` -> `repo`, `cmake -DCB2_RELEASE_REPO=owner/name`, `tools\make_release.ps1 -Repo owner/name`) and copy `docs/index.html`, `style.css`, `app.js`, `config.js`, `.nojekyll` and `img/` there.

## Editing the page

`docs/index.html` (text, including the "Where this is going" section), `docs/style.css`, `docs/config.js` (repository, donation links, amounts and the milestones). Screenshots are in `docs/img/`; replace them or add
footage (a `<video>` in the hero works well). Open `docs/index.html` in a browser to preview it (or `python -m http.server` in `docs/`); the download section reads the latest release live from the GitHub API.

Milestones (`config.js`): `status` is `done`, `active` or `planned`; `goal` is the cumulative amount that has to be raised for it; the bars follow `raised`, which you update by hand.
The current list: online multiplayer (done), native world renderer (in progress), speedrun multiplayer, dedicated servers, native water/ice, co-op mode, skins, level editor, custom levels, freeroam.

## Donations

The page only links to services you set up (Ko-fi, GitHub Sponsors, PayPal ...; an empty `url` hides the button). It says that donations support the development of the open-source tools and unlock nothing. Keep it that way:
the game belongs to its rights holders, so do not sell access, perks inside the game or anything that contains game data, and do not use the game's name for products.
