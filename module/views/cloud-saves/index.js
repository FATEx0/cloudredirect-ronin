var root = null;
var authTimer = null;
var syncTimer = null;

function el(tag, cls, text) {
  var node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
}

function wrenchIcon() {
  var icon = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  icon.setAttribute("viewBox", "0 0 24 24");
  icon.setAttribute("width", "19");
  icon.setAttribute("height", "19");
  icon.setAttribute("fill", "none");
  icon.setAttribute("stroke", "currentColor");
  icon.setAttribute("stroke-width", "2");
  icon.setAttribute("stroke-linecap", "round");
  icon.setAttribute("stroke-linejoin", "round");
  icon.innerHTML = "<path d=\"M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.8-3.8a6 6 0 0 1-7.9 7.9l-6.9 6.9a2.1 2.1 0 0 1-3-3l6.9-6.9a6 6 0 0 1 7.9-7.9z\"/>";
  return icon;
}

function saveIcon() {
  var icon = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  icon.setAttribute("viewBox", "0 0 24 24");
  icon.setAttribute("width", "19");
  icon.setAttribute("height", "19");
  icon.setAttribute("fill", "none");
  icon.setAttribute("stroke", "currentColor");
  icon.setAttribute("stroke-width", "2");
  icon.setAttribute("stroke-linecap", "round");
  icon.setAttribute("stroke-linejoin", "round");
  icon.innerHTML = "<path d=\"M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z\"/>" +
    "<path d=\"M17 21v-8H7v8M7 3v5h8\"/>";
  return icon;
}

function stopAuthPoll() {
  if (authTimer) clearTimeout(authTimer);
  authTimer = null;
}

function stopSyncPoll() {
  if (syncTimer) clearTimeout(syncTimer);
  syncTimer = null;
}

function formatSize(bytes) {
  var value = Number(bytes) || 0;
  if (value < 1024) return value + " B";
  var units = ["KB", "MB", "GB", "TB"], unit = 0;
  do { value /= 1024; unit += 1; } while (value >= 1024 && unit < units.length);
  return (value >= 10 ? Math.round(value) : Math.round(value * 10) / 10) + " " + units[unit - 1];
}

var titleCache = {};

function setMessage(node, text, kind) {
  node.className = "message " + (kind || "");
  node.textContent = text || "";
}

function pollAuth(context, status) {
  stopAuthPoll();
  authTimer = setTimeout(function () {
    context.call("tsuki.cloudredirect.auth.poll", {}).then(function (result) {
      if (result.status === "waiting") return pollAuth(context, status);
      if (result.status === "done") {
        setMessage(status, "Signed in successfully.", "success");
        return load(context);
      }
      setMessage(status, result.status === "timeout" ? "Sign-in timed out." :
        "Sign-in failed: " + (result.error || result.status), "error");
    }).catch(function (error) { setMessage(status, String(error), "error"); });
  }, 1000);
}

function renderApps(context, parent, status) {
  var section = el("section", "game-section");
  var heading = el("div", "section-heading");
  heading.appendChild(el("span", "section-label", "Cloud save data"));
  var count = el("span", "section-count", "…");
  heading.appendChild(count);
  var search = document.createElement("input");
  search.className = "game-search";
  search.placeholder = "Search by app ID";
  var account = document.createElement("select");
  account.className = "account-select";
  account.hidden = true;
  var toolbar = el("div", "page-toolbar");
  toolbar.appendChild(search); toolbar.appendChild(account);
  var list = el("div", "save-list", "Loading cloud saves…");
  section.appendChild(heading); section.appendChild(toolbar); section.appendChild(list);
  parent.appendChild(section);

  var local = [], remote = {}, current = null;
  function draw() {
    var q = search.value.trim();
    var merged = {}, resolved = remote[current] !== undefined;
    local.filter(function (app) { return current == null || app.account === current; })
      .forEach(function (app) {
        merged[app.appid] = { appid: app.appid, files: app.files, size: app.size,
          title: app.title, local: app.local, remote: false,
          manual_configured: app.manual_configured, inactive: app.location === "inactive",
          steam_autocloud: app.steam_autocloud === true };
      });
    Object.keys(remote[current] || {}).forEach(function (id) {
      var item = remote[current][id];
      if (!merged[id]) merged[id] = { appid: Number(id), files: item.files,
        size: item.size, title: item.title, local: false, remote: true };
      else merged[id].remote = true;
    });
    var apps = Object.keys(merged).map(function (id) { return merged[id]; })
      .filter(function (app) {
        var name = (titleCache[app.appid] || "").toLowerCase();
        return (!q || String(app.appid).indexOf(q) >= 0 || name.indexOf(q.toLowerCase()) >= 0)
          && (app.files > 0 || app.remote || app.inactive || app.manual_configured || app.steam_autocloud);
      })
      .sort(function (a, b) { return a.appid - b.appid; });
    list.textContent = ""; count.textContent = String(apps.length);
    if (!apps.length) list.appendChild(el("div", "empty", q ? "No matching games." : "No cloud save data found."));
    apps.forEach(function (app) {
      var card = el("article", "save-card");
      var art = document.createElement("img");
      art.className = "game-art";
      art.alt = "";
      art.loading = "lazy";
      art.decoding = "async";
      art.src = "https://shared.fastly.steamstatic.com/store_item_assets/steam/apps/"
        + encodeURIComponent(app.appid) + "/header.jpg";
      art.addEventListener("error", function () { art.classList.add("missing"); });
      var copy = el("div", "save-copy");
      var title = el("strong", "", app.title || ("Unknown game"));
      copy.appendChild(title);
      copy.appendChild(el("small", "", "AppID " + app.appid + " · " +
        (app.files || 0) + " files · " + formatSize(app.size)));
      var location = app.inactive && !app.manual_configured ? "Inactive" : !resolved ? "Checking…" :
        (app.local && app.remote ? "Synced" : app.remote ? "Cloud" : "This PC");
      var category = app.steam_autocloud ? "Steam Auto-Cloud" :
        app.manual_configured ? "Manual" : app.inactive ? "Unconfigured" : null;
      card.appendChild(art); card.appendChild(copy);
      if (app.steam_autocloud) {
        var badges = el("div", "save-badges");
        badges.appendChild(el("span", "category steam-auto-cloud", category));
        badges.appendChild(el("span", "location " + location.toLowerCase().replace(/[^a-z]+/g, "-"), location));
        card.appendChild(badges);
      } else if (app.inactive || app.manual_configured) {
        var configure = el("button", "icon-action");
        configure.type = "button";
        configure.appendChild(wrenchIcon());
        configure.title = app.manual_configured ? "Change save folder" : "Configure save folder";
        configure.setAttribute("aria-label", configure.title);
        configure.addEventListener("click", function () {
          configure.disabled = true;
          context.call("tsuki.cloudredirect.manual.candidates", {app_id: app.appid}).then(function (result) {
            if (!result || result.success === false) throw new Error(result && result.error || "candidate scan failed");
            var choices = Array.isArray(result.candidates) ? result.candidates : [];
            if (!choices.length) throw new Error("No bounded save-folder candidates found.");
            var editor = el("div", "manual-editor");
            var choicesList = el("div", "manual-choices");
            var configured = result.configured && Array.isArray(result.configured.rules) ?
              result.configured.rules : [];
            function isConfigured(candidate) {
              return configured.some(function (rule) {
                return rule.root === candidate.root && rule.path === candidate.path;
              });
            }
            choices.forEach(function (candidate) {
              var row = el("label", "manual-choice");
              var checkbox = document.createElement("input");
              checkbox.type = "checkbox";
              checkbox.checked = isConfigured(candidate) || (!configured.length && choices.length === 1);
              checkbox._candidate = candidate;
              row.appendChild(checkbox);
              var label = el("span", "");
              label.appendChild(el("strong", "", candidate.path));
              label.appendChild(el("small", "", candidate.files + " files · " + formatSize(candidate.size)));
              row.appendChild(label); choicesList.appendChild(row);
            });
            var accept = el("button", "icon-action save-action");
            accept.type = "button";
            accept.title = "Save selected folders";
            accept.setAttribute("aria-label", accept.title);
            accept.appendChild(saveIcon());
            accept.addEventListener("click", function () {
              var locations = [].slice.call(choicesList.querySelectorAll("input:checked"))
                .map(function (input) { return {
                  root: input._candidate.root, path: input._candidate.path
                }; });
              if (!locations.length) {
                editor.classList.add("error");
                return void editor.appendChild(el("small", "selection-error", "Select at least one folder."));
              }
              accept.disabled = true;
              context.call("tsuki.cloudredirect.manual.configure", {
                app_id: app.appid, locations: locations, enabled: true
              }).then(function (saved) {
                if (!saved || saved.success === false) throw new Error(saved && saved.error || "configuration failed");
                editor.textContent = "Active now" +
                  (saved.files !== undefined ? " · " + saved.files + " local files synced." : ".");
                setTimeout(function () { if (root) load(context); }, 900);
              }).catch(function (error) { accept.disabled=false; editor.textContent=String(error); });
            });
            editor.appendChild(choicesList); editor.appendChild(accept);
            card.appendChild(editor);
          }).catch(function (error) {
            configure.disabled=false;
            card.appendChild(el("div", "manual-editor error", String(error)));
          });
        });
        if (category) {
          var manualBadges = el("div", "save-badges");
          manualBadges.appendChild(el("span", "category " +
            (app.manual_configured ? "manual" : "unconfigured"), category));
          manualBadges.appendChild(el("span", "location " + location.toLowerCase().replace(/[^a-z]+/g, "-"), location));
          card.appendChild(manualBadges);
        }
        card.appendChild(configure);
      } else {
        card.appendChild(el("span", "location " + location.toLowerCase().replace(" ", "-"), location));
      }
      list.appendChild(card);
    });
  }
  function fetchRemote() {
    if (current == null || remote[current] !== undefined || status.provider === "local") {
      if (status.provider === "local" && current != null) remote[current] = {};
      return draw();
    }
    var ids = local.filter(function (app) { return app.account === current; })
      .map(function (app) { return app.appid; });
    context.call("tsuki.cloudredirect.remote.list", {account: current, local_appids: ids})
      .then(function (result) {
        var set = {};
        (Array.isArray(result.apps) ? result.apps : []).forEach(function (app) {
          set[Number(app.appid)] = app;
          if (app.title) titleCache[Number(app.appid)] = app.title;
        });
        remote[current] = set; draw();
      }).catch(function () { remote[current] = {}; draw(); });
  }
  search.addEventListener("input", draw);
  account.addEventListener("change", function () { current = Number(account.value); draw(); fetchRemote(); });
  context.call("tsuki.cloudredirect.apps.list", {}).then(function (result) {
    local = Array.isArray(result.apps) ? result.apps : [];
    local.forEach(function (app) { if (app.title) titleCache[app.appid] = app.title; });
    var accounts = Array.isArray(result.accounts) ? result.accounts : [];
    if (accounts.length) current = accounts[0].id;
    if (accounts.length > 1) {
      account.hidden = false;
      accounts.forEach(function (item) {
        var option = el("option", "", item.name || ("Account " + item.id));
        option.value = item.id; account.appendChild(option);
      });
    }
    draw(); fetchRemote();
  }).catch(function (error) { list.textContent = "Unable to load cloud saves: " + String(error); });
}

function render(context, status) {
  stopAuthPoll(); stopSyncPoll(); root.textContent = "";
  var config = el("section", "config-card");
  var rows = el("div", "setting-rows");
  var syncRow = el("div", "setting-row");
  var syncCopy = el("span", "setting-copy");
  syncCopy.appendChild(el("strong", "", "Force Sync"));
  syncCopy.appendChild(el("small", "", "Reconcile CloudRedirect's cached saves with the provider now."));
  var sync = el("button", "action", "Sync now");
  var syncStatus = el("div", "message");
  sync.disabled = status.provider === "local" || !status.authenticated;
  sync.addEventListener("click", function () {
    sync.disabled = true;
    setMessage(syncStatus, "Starting provider sync…", "working");
    context.call("tsuki.cloudredirect.sync.begin", {}).then(function (result) {
      if (!result || result.success === false) throw new Error(result && result.error || "sync failed");
      var requestId = result.request_id;
      var started = Date.now();
      function poll() {
        context.call("tsuki.cloudredirect.sync.status", {request_id: requestId}).then(function (state) {
          if (state.status === "done") {
            sync.disabled = false;
            setMessage(syncStatus, "Sync complete" + (state.count ? " (" + state.count + " apps checked)." : "."), "success");
            var currentApps = root && root.querySelector(".game-section");
            if (currentApps) currentApps.remove();
            if (root) renderApps(context, root, status);
            return;
          }
          if (state.status === "error") throw new Error(state.error || "sync failed");
          if (Date.now() - started > 120000) throw new Error("sync timed out");
          setMessage(syncStatus, state.status === "working" ? "Synchronizing saves…" : "Waiting for CloudRedirect…", "working");
          syncTimer = setTimeout(poll, 500);
        }).catch(function (error) {
          sync.disabled = false; setMessage(syncStatus, String(error), "error");
        });
      }
      poll();
    }).catch(function (error) {
      sync.disabled = false; setMessage(syncStatus, String(error), "error");
    });
  });
  syncRow.appendChild(syncCopy); syncRow.appendChild(sync);
  var providerRow = el("div", "setting-row");
  var providerCopy = el("span", "setting-copy");
  var providerNames = {local:"Local only", gdrive:"Google Drive", onedrive:"OneDrive"};
  providerCopy.appendChild(el("strong", "", providerNames[status.provider] || status.provider));
  providerCopy.appendChild(el("small", "", "Change the storage provider from the Settings page."));
  providerRow.appendChild(providerCopy); rows.appendChild(providerRow);
  var authStatus = el("div", "message");
  if (status.provider !== "local") {
    var authRow = el("div", "setting-row");
    var authCopy = el("span", "setting-copy");
    authCopy.appendChild(el("strong", "", status.authenticated ? "Connected" : "Not connected"));
    authCopy.appendChild(el("small", "", status.authenticated ? "Provider credentials are available." : "Sign in to sync remote saves."));
    var auth = el("button", "action " + (status.authenticated ? "secondary" : ""), status.authenticated ? "Sign out" : "Sign in");
    auth.addEventListener("click", function () {
      auth.disabled = true;
      if (status.authenticated) {
        context.call("tsuki.cloudredirect.auth.signout", {provider: status.provider}).then(function () { load(context); });
      } else {
        setMessage(authStatus, "Waiting for browser sign-in…", "working");
        context.call("tsuki.cloudredirect.auth.begin", {provider: status.provider}).then(function (result) {
          return context.call("tsuki.desktop.url.open", {url: result.auth_url});
        }).then(function () { pollAuth(context, authStatus); })
          .catch(function (error) { auth.disabled = false; setMessage(authStatus, String(error), "error"); });
      }
    });
    authRow.appendChild(authCopy); authRow.appendChild(auth); rows.appendChild(authRow);
  }
  rows.appendChild(syncRow);
  config.appendChild(rows); config.appendChild(syncStatus); config.appendChild(authStatus); root.appendChild(config);
  renderApps(context, root, status);
}

function load(context) {
  root.textContent = "Loading cloud settings…";
  context.call("tsuki.cloudredirect.status", {}).then(function (status) {
    if (!status || status.success === false) throw new Error(status && status.error || "status failed");
    render(context, status);
  }).catch(function (error) { root.textContent = "Unable to load cloud settings: " + String(error); });
}

function mount(context) { root = context.container; root.className = "cloud-saves-page"; load(context); }
function unmount() { stopAuthPoll(); stopSyncPoll(); if (root) root.textContent = ""; root = null; }
