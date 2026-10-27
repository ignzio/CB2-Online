(function () {
  var cfg = window.CB2_SITE;
  var $ = function (id) { return document.getElementById(id); };
  var repoUrl = "https://github.com/" + cfg.repo;

  $("repo-link").href = repoUrl + "/releases";
  $("issues-link").href = repoUrl + "/issues";
  $("dl-all").href = repoUrl + "/releases";
  $("dl-fallback").href = repoUrl + "/releases/latest";
  $("dl-zip").href = repoUrl + "/releases/latest";

  function money(n) {
    try { return new Intl.NumberFormat(undefined, { style: "currency", currency: cfg.currency, maximumFractionDigits: 0 }).format(n); }
    catch (e) { return n + " " + cfg.currency; }
  }
  function mb(bytes) { return (bytes / 1048576).toFixed(1) + " MB"; }

  // latest release (the GitHub API answers without a login for public repositories)
  fetch("https://api.github.com/repos/" + cfg.repo + "/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (rel) {
      var version = (rel.tag_name || "").replace(/^v/, "");
      $("hero-version").textContent = "v" + version;
      $("dl-version").textContent = "v" + version;
      if (rel.published_at) $("dl-date").textContent = "(" + new Date(rel.published_at).toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" }) + ")";
      var zip = (rel.assets || []).filter(function (a) { return /\.zip$/i.test(a.name); })[0];
      if (zip) {
        $("dl-zip").href = zip.browser_download_url;
        $("dl-size").textContent = zip.name + " - " + mb(zip.size) + " - " + (zip.download_count || 0) + " downloads";
      }
      $("dl-notes").textContent = (rel.body || "").trim() || "See the release page for details.";
    })
    .catch(function () {
      $("dl-error").hidden = false;
      $("dl-notes").textContent = "";
      $("dl-version").textContent = "";
    });

  // what GitHub Sponsors has brought in (docs/sponsors.json, refreshed by a scheduled workflow); added to the hand-typed amount
  var raised = cfg.raised || 0;
  function showMilestones() {
  var box = $("milestones");
  box.innerHTML = "";
  cfg.milestones.forEach(function (m) {
    var pct = m.status === "done" ? 100 : m.goal > 0 ? Math.max(0, Math.min(100, Math.round(raised / m.goal * 100))) : (m.status === "active" ? 100 : 0);
    var el = document.createElement("article");
    el.className = "ms " + m.status;
    var funded = m.status === "planned" && m.goal > 0 && raised >= m.goal;
    var tag = m.status === "done" ? "Done" : m.status === "active" ? "In progress" : funded ? "Funded" : "Planned";
    el.innerHTML = '<header><h3></h3><span class="tag">' + tag + '</span></header><p></p><div class="bar"><i></i></div><div class="bar-label"></div>';
    el.querySelector("h3").textContent = m.title;
    el.querySelector("p").textContent = m.text;
    el.querySelector(".bar i").style.width = pct + "%";
    el.querySelector(".bar-label").textContent = m.status === "done" ? "Delivered" : m.goal > 0 ? money(Math.min(raised, m.goal)) + " of " + money(m.goal) : "Funded by the project";
    box.appendChild(el);
  });
  $("raised").textContent = money(raised);
  }
  showMilestones();
  fetch("sponsors.json", { cache: "no-cache" })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (s) {
      if (typeof s.raised !== "number") return;
      raised = (cfg.raised || 0) + s.raised;
      showMilestones();
      if (s.updated) $("raised-note").textContent = "Updated " + new Date(s.updated).toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" }) + " from GitHub Sponsors: one-time sponsorships plus current monthly pledges.";
    })
    .catch(function () { /* no sponsors.json yet: the typed-in amount is shown */ });

  // donation buttons
  var buttons = $("donate-buttons"), shown = 0;
  cfg.donate.forEach(function (d) {
    if (!d.url) return;
    if (d.iframe) {   // GitHub's own Sponsor button
      var f = document.createElement("iframe");
      f.src = d.iframe;
      f.title = "Sponsor ignzio";
      f.height = "32";
      f.width = "114";
      f.style.cssText = "border: 0; border-radius: 6px;";
      buttons.appendChild(f);
      ++shown;
      return;
    }
    var a = document.createElement("a");
    a.className = "btn big";
    a.href = d.url;
    a.rel = "noopener";
    a.textContent = "Support on " + d.name;
    a.title = d.note || "";
    buttons.appendChild(a);
    ++shown;
  });
  if (shown) $("donate-empty").hidden = true;
})();
