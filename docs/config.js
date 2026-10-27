// Everything you will want to change on the page lives here.
window.CB2_SITE = {
  // The PUBLIC repository that holds the releases (and the issue tracker). The source repository can stay private.
  repo: "ignzio/CB2-Online",

  // Donation buttons. Leave "url" empty to hide a button. Replace the placeholders with your own pages.
  donate: [
    { name: "Ko-fi", url: "", note: "one-off or monthly" },
    { name: "GitHub Sponsors", url: "https://github.com/sponsors/ignzio", iframe: "https://github.com/sponsors/ignzio/button", note: "monthly or one-off" },
    { name: "PayPal", url: "", note: "one-off" }
  ],
  // What was raised outside GitHub Sponsors (Ko-fi, PayPal ...), typed in by hand. GitHub Sponsors is read automatically: docs/sponsors.json is refreshed every few hours by
  // .github/workflows/sponsors.yml (see docs/WEBSITE.md) and added to this number. Both only drive the progress bars. GitHub Sponsors amounts are in US dollars.
  raised: 0,
  currency: "USD",

  // Milestones. status: "done" | "active" | "planned". "goal" is the total that has to be raised for it (cumulative).
  milestones: [
    { title: "Online multiplayer, lobby and both releases", status: "done", goal: 0,
      text: "Other players inside your game, an online lobby with passwords, and the European (PAL) and American (NTSC-U) releases." },
    { title: "Native world renderer", status: "active", goal: 0,
      text: "The level scenery drawn from the level files: true widescreen, exact 60/160 fps camera, and the base for the editor and custom levels. Unstable in the current release." },
    { title: "Speedrun multiplayer", status: "planned", goal: 150,
      text: "The full racing experience: synchronised starts, live splits and standings for the whole room, ghosts of the best runs, and public leaderboards with a page per level." },
    { title: "Dedicated lobby and relay servers", status: "planned", goal: 300,
      text: "Reliable servers in Europe and North America, so rooms are found instantly and play works behind strict routers and phone hotspots." },
    { title: "Water, ice and animated textures in the native renderer", status: "planned", goal: 450,
      text: "See-through and animated level surfaces drawn natively as well, so the whole picture can be widened and upscaled cleanly." },
    { title: "Co-op mode", status: "planned", goal: 650,
      text: "Really playing together, not only seeing each other: shared crates, enemies and checkpoints, so two or more players can clear a level as a team." },
    { title: "Skins", status: "planned", goal: 800,
      text: "Choose how your Crash looks - colours and alternative models - and everybody in the room sees it, in every level." },
    { title: "Level editor", status: "planned", goal: 1000,
      text: "Build and change levels visually: place crates, enemies and platforms and edit the collision, on top of the native world renderer." },
    { title: "Custom levels", status: "planned", goal: 1250,
      text: "Play levels that are not on the disc, and share them: a room downloads the level its host picked, and a community gallery to browse." },
    { title: "Freeroam", status: "planned", goal: 1500,
      text: "A free-exploration mode with everybody in the same open world: roam between levels and the warp room without the game's progress rules, together." }
  ]
};
