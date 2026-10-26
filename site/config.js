// Everything you will want to change on the page lives here.
window.CB2_SITE = {
  // The PUBLIC repository that holds the releases (and the issue tracker). The source repository can stay private.
  repo: "ignzio/CB2-Online",

  // Donation buttons. Leave "url" empty to hide a button. Replace the placeholders with your own pages.
  donate: [
    { name: "Ko-fi", url: "", note: "one-off or monthly" },
    { name: "GitHub Sponsors", url: "", note: "monthly" },
    { name: "PayPal", url: "", note: "one-off" }
  ],
  // Update this number by hand when donations arrive (in the currency below). It only drives the progress bars.
  raised: 0,
  currency: "EUR",

  // Milestones. status: "done" | "active" | "planned". "goal" is the total that has to be raised for it (cumulative).
  milestones: [
    { title: "Native world renderer", status: "active", goal: 0,
      text: "The level scenery drawn from the level files: true widescreen, exact 60/160 fps camera, and the base for custom levels. Experimental in the current release." },
    { title: "Online leaderboards", status: "planned", goal: 150,
      text: "A server that keeps level times, so records are shared and compared worldwide, with a public page per level." },
    { title: "Dedicated lobby and relay servers", status: "planned", goal: 300,
      text: "Reliable servers in Europe and North America, so rooms are found instantly and play works behind strict routers." },
    { title: "Water, ice and animated textures in the native renderer", status: "planned", goal: 450,
      text: "See-through and animated level surfaces drawn natively as well, so the whole picture can be widened and upscaled cleanly." },
    { title: "Custom levels and a level editor", status: "planned", goal: 800,
      text: "Load levels that are not on the disc: an editor, collision, and a way to share them with the room." }
  ]
};
