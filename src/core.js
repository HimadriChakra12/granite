// ---- core.js -----------------------------------------------------------
// Runs for real, in the browser, as part of the userscript. Everything a
// site script declared (via define/url/focus/click/loop/goto in the DSL)
// arrives here already compiled down to plain data + real functions, via
// Sites.register({...}) calls appended right after this file.
// --------------------------------------------------------------------

var Sites = {
	list: [],
	register: function (site) { Sites.list.push(site); }
};

// Built-in fallback bindings, used only for key-sequences a site doesn't
// define itself. Site bindings always win on a collision.
var DEFAULT_BINDINGS = [
	{ keys: "gi", action: "focus", kind: "function", value: function () { focusFirstInput(false); } },
	{ keys: "gI", action: "focus", kind: "function", value: function () { focusFirstInput(true); } }
];

function isVisible(el) {
	if (!el) return false;
	var r = el.getBoundingClientRect();
	return r.width > 0 && r.height > 0 && el.offsetParent !== null;
}

function focusFirstInput(fromEnd) {
	var els = Array.prototype.slice.call(
		document.querySelectorAll('input:not([type=hidden]):not([disabled]), textarea:not([disabled]), [contenteditable="true"]')
	).filter(isVisible);
	if (!els.length) return;
	var el = fromEnd ? els[els.length - 1] : els[0];
	el.focus();
}

// ---- URL matching (Tampermonkey-style glob, e.g. "*://kagi.com/*") -----

function globToRegExp(glob) {
	var escaped = glob.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*");
	return new RegExp("^" + escaped + "$");
}

function siteMatches(site) {
	return (site.match || []).some(function (pattern) {
		return globToRegExp(pattern).test(location.href);
	});
}

// A site with NO url() calls at all (match: []) is "universal" -- it
// applies on every page, not just where its own url() patterns hit.
function isUniversal(site) {
	return !(site.match && site.match.length);
}

// The one specific (non-universal) site whose url() patterns match this
// page, if any. Still a single-winner model, same as before.
function activeSite() {
	for (var i = 0; i < Sites.list.length; i++) {
		var site = Sites.list[i];
		if (!isUniversal(site) && siteMatches(site)) return site;
	}
	return null;
}

function universalSites() {
	return Sites.list.filter(isUniversal);
}

// Precedence, highest first: the active specific site's own bindings,
// then every universal site's bindings (as a fallback layer any site
// can share), then the built-in gi/gI defaults. A key already claimed
// by an earlier layer is never overridden by a later one.
// Within ONE site's own bindings list, a later entry for the same key
// overrides an earlier one -- e.g. off(action(r)) written after
// action(r, reload) correctly cancels it, rather than the two just
// coexisting with the first (real) one winning by accident.
function dedupeLastWins(bindings) {
	var byKey = {};
	var order = [];
	bindings.forEach(function (b) {
		if (!byKey.hasOwnProperty(b.keys)) order.push(b.keys);
		byKey[b.keys] = b; // last one written wins
	});
	return order.map(function (k) { return byKey[k]; });
}

function effectiveBindings() {
	var seen = {};
	var result = [];
	function addAll(bindings) {
		bindings.forEach(function (b) {
			if (!seen[b.keys]) { seen[b.keys] = true; result.push(b); }
		});
	}

	var specific = activeSite();
	if (specific) addAll(dedupeLastWins(specific.bindings));
	universalSites().forEach(function (site) { addAll(dedupeLastWins(site.bindings)); });
	addAll(DEFAULT_BINDINGS);

	return result;
}

// Same precedence as effectiveBindings, for loop()'s selectors: the
// active specific site's own loops win, universal sites fill any gaps.
function findLoopSelector(loopName) {
	var specific = activeSite();
	if (specific && specific.loops && specific.loops[loopName]) return specific.loops[loopName];
	var universal = universalSites();
	for (var i = 0; i < universal.length; i++) {
		if (universal[i].loops && universal[i].loops[loopName]) return universal[i].loops[loopName];
	}
	return null;
}

// ---- loop / goto cursor --------------------------------------------------

var loopCursor = {};       // loopName -> the actual highlighted Element (not an index -- see gotoLoop)
var lastHighlighted = null;

// Resolves selected(...)'s loop-name list into actual elements: every
// loop's current cursor item if no names were given, or just the named
// ones. Filters out loops that have never been visited yet (no cursor
// set) or whose element has since left the DOM entirely.
function resolveSelected(loopNames) {
	var names = (loopNames && loopNames.length) ? loopNames : Object.keys(loopCursor);
	return names.map(function (n) { return loopCursor[n]; })
		.filter(function (el) { return el && el.isConnected; });
}

// Resolves yankurl()'s optional target into an actual URL string.
// No target at all -> the current page's URL. A resolved element with
// no direct link -> its closest/nested <a href>, if any. Falls back to
// the current page's URL if nothing usable is found, rather than
// copying nothing.
function resolveYankTarget(b) {
	if (!b.hasTarget) return null;
	if (b.targetKind === "selector") return document.querySelector(b.targetValue);
	if (b.targetKind === "goto") return gotoLoop(b.targetLoop, b.targetDir);
	if (b.targetKind === "selected") {
		var els = resolveSelected(b.targetSelLoops);
		return els.length ? els[0] : null;
	}
	if (b.targetKind === "function") return b.targetValue(document.activeElement, MU);
	return null;
}

function resolveYankUrl(b) {
	var target = resolveYankTarget(b);
	if (!target) return location.href;
	if (typeof target === "string") return target; // a custom function returned a URL directly
	if (target.tagName === "A" && target.href) return target.href;
	if (target.closest) {
		var ancestorLink = target.closest("a[href]");
		if (ancestorLink) return ancestorLink.href;
	}
	if (target.querySelector) {
		var innerLink = target.querySelector("a[href]");
		if (innerLink) return innerLink.href;
	}
	return location.href;
}

// ---- self-owned smooth scrolling ------------------------------------------
//
// Deliberately NOT using the browser's native `behavior: "smooth"`. Native
// smooth scrolls don't reliably cancel/retarget when a new one starts
// before the last one finishes -- especially on custom scroll containers
// (Instagram's feed, Spotify's list) -- so rapid j/k presses stacked
// multiple animations and produced exactly the overshoot/"catches up
// late" glitching seen before. This tiny eased rAF loop is fully owned:
// every call cancels whatever's still running first, so there's always
// at most one animation in flight, ever -- smooth to look at, but never
// stackable.
var scrollAnimId = null;

function easeOutCubic(t) { return 1 - Math.pow(1 - t, 3); }

function animateScrollTop(container, targetTop, duration) {
	if (scrollAnimId) cancelAnimationFrame(scrollAnimId);
	var startTop = container.scrollTop;
	var delta = targetTop - startTop;
	var startTime = null;

	function step(ts) {
		if (startTime === null) startTime = ts;
		var t = Math.min((ts - startTime) / duration, 1);
		container.scrollTop = startTop + delta * easeOutCubic(t);
		scrollAnimId = (t < 1) ? requestAnimationFrame(step) : null;
	}
	scrollAnimId = requestAnimationFrame(step);
}

// Nearest ancestor that actually scrolls, or the page itself.
function scrollParentOf(el) {
	var node = el.parentElement;
	while (node) {
		var style = getComputedStyle(node);
		if (/(auto|scroll)/.test(style.overflowY) && node.scrollHeight > node.clientHeight) return node;
		node = node.parentElement;
	}
	return document.scrollingElement || document.documentElement;
}

function highlight(el) {
	if (lastHighlighted && lastHighlighted !== el) {
		lastHighlighted.classList.remove("mu-cursor");
	}
	if (el) {
		el.classList.add("mu-cursor");

		var container = scrollParentOf(el);
		var elRect = el.getBoundingClientRect();
		var containerRect = (container === document.scrollingElement || container === document.documentElement)
			? { top: 0, height: window.innerHeight }
			: container.getBoundingClientRect();
		// same target position "block: center" would compute
		var delta = (elRect.top - containerRect.top) - (containerRect.height / 2 - elRect.height / 2);
		animateScrollTop(container, container.scrollTop + delta, 180);
	}
	lastHighlighted = el;
}

// dir is clamped, not wrapped, by default -- change Math.min/Math.max
// below to wrap instead if you'd rather j on the last result cycle back
// to the first.
//
// Tracks the actual highlighted ELEMENT, not a numeric index. Many
// real lists (Instagram's conversation list, Spotify's track list) are
// virtualized -- only a small, constantly-changing window of rows
// exists in the DOM at any moment, recycled as you scroll. A plain
// index into querySelectorAll's results silently pointed at a
// different-sized array on every single press, which is what looked
// like j/k "jumping twice" or overshooting. Re-locating the previous
// element in the fresh results (and falling back cleanly to an edge if
// it's been recycled out) is stable regardless of how the underlying
// list reshuffles.
function gotoLoop(loopName, dir) {
	var selector = findLoopSelector(loopName);
	if (!selector) return null;

	var els = Array.prototype.slice.call(document.querySelectorAll(selector));
	if (!els.length) return null;

	var prevEl = loopCursor.hasOwnProperty(loopName) ? loopCursor[loopName] : null;
	var idx = prevEl ? els.indexOf(prevEl) : -1;
	var recycled = !!prevEl && idx === -1; // had a target, but it's no longer in the DOM/set

	if (idx === -1) idx = (dir === "next") ? -1 : els.length; // lands on the right edge after +1/-1 below
	if (dir === "next") idx = Math.min(idx + 1, els.length - 1);
	else idx = Math.max(idx - 1, 0);

	var el = els[idx];
	loopCursor[loopName] = el;

	console.log("[site-vim] gotoLoop(%s, %s): %d matched elements, idx -> %d%s, target =",
		loopName, dir, els.length, idx, recycled ? " (previous target was recycled out of the DOM)" : "", el);
	highlight(el);
	return el;
}

// ---- action performers ---------------------------------------------------

function fireMouseEvent(el, type) {
	el.dispatchEvent(new MouseEvent(type, { bubbles: true, cancelable: true, view: window }));
}

function doFocus(el) { if (el && el.focus) el.focus(); }
function doClick(el) { if (el) el.click(); }
function doLongpress(el) {
	if (!el) return;
	fireMouseEvent(el, "mousedown");
	setTimeout(function () {
		fireMouseEvent(el, "mouseup");
		el.click();
	}, 600);
}
function doDoubleclick(el) {
	if (!el) return;
	el.click();
	fireMouseEvent(el, "dblclick");
}

function doScroll(dir, amount) {
	var container = document.scrollingElement || document.documentElement;
	if (amount === Infinity) {
		var maxTop = container.scrollHeight - container.clientHeight;
		animateScrollTop(container, dir === "down" ? maxTop : 0, 220);
		return;
	}
	var px = window.innerHeight * (amount / 100);
	animateScrollTop(container, container.scrollTop + (dir === "down" ? px : -px), 180);
}

function doOpenNew(el) {
	if (!el) return;
	var href = (el.tagName === "A" && el.href) ? el.href
		: (el.closest && el.closest("a[href]") ? el.closest("a[href]").href
		: (el.querySelector && el.querySelector("a[href]") ? el.querySelector("a[href]").href : null));
	if (href) window.open(href, "_blank", "noopener");
}

var ACTIONS = { focus: doFocus, click: doClick, longpress: doLongpress, doubleclick: doDoubleclick, opennew: doOpenNew };

// A helper surface handed to site-defined functions (like ANIMATE) so
// they can reuse the same primitives core.js uses internally.
var MU = { gotoLoop: gotoLoop, highlight: highlight, isVisible: isVisible };

function performBinding(b) {
	if (b.kind === "function") {
		// The function owns the whole action -- it decides what to find
		// and what to do with it. The `action` verb is not applied on
		// top of it.
		b.value(document.activeElement, MU);
		return;
	}
	if (b.kind === "url") {
		location.href = b.value; // works for absolute and relative URLs
		return;
	}
	if (b.kind === "scroll") {
		doScroll(b.dir, b.amount);
		return;
	}
	if (b.kind === "navigate") {
		if (b.dir === "prev") history.back();
		else history.forward();
		return;
	}
	if (b.kind === "action") {
		if (b.dir === "reload") location.reload();
		else if (b.dir === "close") {
			// Privileged via the "window.close" grant declared in
			// build.c (Tampermonkey/Violentmonkey back this with
			// their own extension internals) -- unlike ordinary
			// page-JS window.close(), this actually closes the tab
			// regardless of how it was opened or its navigation
			// history.
			window.close();
		}
		return;
	}
	if (b.kind === "root") {
		location.href = location.origin;
		return;
	}
	if (b.kind === "branch") {
		var path = location.pathname;
		if (path.length > 1 && path.charAt(path.length - 1) === "/") path = path.slice(0, -1);
		var upIdx = path.lastIndexOf("/");
		location.href = location.origin + (upIdx > 0 ? path.slice(0, upIdx) : "/");
		return;
	}
	if (b.kind === "yankurl") {
		// Privileged via the "GM_setClipboard" grant declared in
		// build.c -- sidesteps the focus/permission flakiness the
		// plain navigator.clipboard API can hit.
		var yankedUrl = resolveYankUrl(b);
		GM_setClipboard(yankedUrl);
		showYankPopup(yankedUrl);
		return;
	}
	if (b.kind === "off") {
		// Deliberately does nothing. Its whole purpose is just to
		// exist and claim this key at the specific-site layer, so
		// effectiveBindings()'s merge never lets a universal site (or
		// the built-in gi/gI defaults) fill it back in underneath.
		return;
	}
	if (b.kind === "selected") {
		// Applies the action to MULTIPLE elements at once -- every
		// named loop's current cursor item (or every loop's, if none
		// were named). Unlike every other kind, this doesn't resolve
		// to a single `el`.
		var act = ACTIONS[b.action];
		if (act) resolveSelected(b.loops).forEach(act);
		return;
	}
	var el = null;
	if (b.kind === "selector") el = document.querySelector(b.value);
	else if (b.kind === "goto") el = gotoLoop(b.loop, b.dir);
	if (!el) return;
	var act = ACTIONS[b.action];
	if (act) act(el);
}

// ---- multi-key sequence engine -------------------------------------------

var keyBuffer = "";
var keyTimer = null;
var SEQUENCE_TIMEOUT_MS = 1000;

// Throttles repeats of the SAME binding firing again too soon --
// e.g. mashing j/k faster than a goto()'s cursor jump can settle.
// Only ever compares against the immediately-preceding fire, so typing
// a different key, or the same key again after the window has passed,
// is completely unaffected.
var lastFiredKeys = null;
var lastFiredTime = 0;
var MIN_REPEAT_MS = 120;

// Named keys whose event.key isn't a single printable character, mapped
// to a lowercase DSL-friendly token -- so `focus(enter, ...)`,
// `click(space, ...)` etc. actually match. Single characters (g, G, i,
// I, ...) are left completely alone and stay case-sensitive, since shift
// state is the whole point there.
var NAMED_KEYS = {
	" ": "space",
	"Enter": "enter",
	"Tab": "tab",
	"Backspace": "backspace",
	"Delete": "delete",
	"ArrowUp": "up",
	"ArrowDown": "down",
	"ArrowLeft": "left",
	"ArrowRight": "right",
	"Home": "home",
	"End": "end",
	"PageUp": "pageup",
	"PageDown": "pagedown",
	"Insert": "insert"
};

// Bare modifier keydowns (pressing Shift on its own, etc.) carry no
// useful signal and would otherwise pollute the key buffer.
var IGNORED_RAW_KEYS = {
	Shift: 1, Control: 1, Alt: 1, Meta: 1, CapsLock: 1,
	AltGraph: 1, NumLock: 1, ScrollLock: 1, ContextMenu: 1
};

function normalizeKey(rawKey) {
	if (NAMED_KEYS.hasOwnProperty(rawKey)) return NAMED_KEYS[rawKey];
	if (rawKey.length === 1) return rawKey;
	return rawKey.toLowerCase(); // any other named key not listed above (F1, etc.)
}

function isEditableTarget(el) {
	if (!el) return false;
	var tag = el.tagName;
	return el.isContentEditable || tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
}

// document.activeElement stops at a shadow host -- for a real input
// living inside an OPEN shadow root (e.g. a Ctrl+K search overlay built
// with attachShadow({mode:'open'})), activeElement reports the host
// <div>, never the actual focused <input> inside it. Descending through
// .shadowRoot.activeElement (recursively, in case of nested shadow
// trees) finds the real focused element instead.
function getDeepActiveElement() {
	var el = document.activeElement;
	while (el && el.shadowRoot && el.shadowRoot.activeElement) {
		el = el.shadowRoot.activeElement;
	}
	return el;
}

function resetKeyBuffer() {
	keyBuffer = "";
	if (keyTimer) { clearTimeout(keyTimer); keyTimer = null; }
}

// Clears every loop's cursor and removes the highlight -- Escape uses
// this to fully "break the loop," not just exit insert mode.
function clearAllHighlights() {
	if (lastHighlighted) {
		lastHighlighted.classList.remove("mu-cursor");
		lastHighlighted = null;
	}
	loopCursor = {};
}

document.addEventListener("keydown", function (ev) {
	// event.target is retargeted to the shadow HOST for any listener
	// outside the shadow tree -- same blind spot as activeElement, same
	// fix: composedPath()[0] is the actual originating element,
	// piercing an open shadow boundary (a closed one hides it by the
	// shadow author's own deliberate choice, which nothing here can or
	// should override).
	var realTarget = (typeof ev.composedPath === "function" && ev.composedPath()[0]) || ev.target;

	// Checked against ev.target, document.activeElement, AND their
	// shadow-DOM-aware equivalents -- rich-text editors (Instagram's
	// Lexical editor is one) do brief internal focus/blur churn where
	// plain ev.target/activeElement can momentarily disagree, and a
	// shadow-DOM overlay (Instagram's own Ctrl+K search, for one) hides
	// the real focused element from both entirely unless you descend
	// into shadowRoot.activeElement.
	var deepActive = getDeepActiveElement();
	var editing = isEditableTarget(realTarget) || isEditableTarget(ev.target) ||
		isEditableTarget(document.activeElement) || isEditableTarget(deepActive);

	// Escape always exits "insert mode" AND breaks any active loop/goto
	// cursor -- checked before the editable-target bailout below, since
	// that's precisely when it's needed.
	if (ev.key === "Escape") {
		if (editing) {
			if (deepActive && deepActive.blur) deepActive.blur();
			if (document.activeElement && document.activeElement.blur) document.activeElement.blur();
			if (ev.target && ev.target.blur) ev.target.blur();
		}
		clearAllHighlights();
		resetKeyBuffer();
		return;
	}

	if (ev.altKey || ev.ctrlKey || ev.metaKey) return;
	if (editing) return; // don't hijack typing; gi/gI got you here
	if (IGNORED_RAW_KEYS[ev.key]) return;

	var key = normalizeKey(ev.key);
	var candidate = keyBuffer + key;
	var bindings = effectiveBindings();

	var exact = bindings.filter(function (b) { return b.keys === candidate; });
	var stillPossible = bindings.some(function (b) { return b.keys.indexOf(candidate) === 0; });

	if (exact.length) {
		ev.preventDefault();
		var now = Date.now();
		var tooSoon = candidate === lastFiredKeys && (now - lastFiredTime) < MIN_REPEAT_MS;
		console.log("[site-vim] key=%s (raw=%s) candidate=%s repeat=%s -> %s",
			key, ev.key, candidate, ev.repeat,
			tooSoon ? "THROTTLED (" + (now - lastFiredTime) + "ms since last fire)"
			        : "fired: " + JSON.stringify(exact[0]));
		if (!tooSoon) {
			performBinding(exact[0]);
			lastFiredKeys = candidate;
			lastFiredTime = now;
		}
		resetKeyBuffer();
		return;
	}

	if (stillPossible) {
		console.log("[site-vim] key=%s (raw=%s) candidate=%s -> buffering (waiting for more keys)", key, ev.key, candidate);
		ev.preventDefault();
		keyBuffer = candidate;
		if (keyTimer) clearTimeout(keyTimer);
		keyTimer = setTimeout(resetKeyBuffer, SEQUENCE_TIMEOUT_MS);
	} else {
		if (candidate.length) console.log("[site-vim] key=%s (raw=%s) candidate=%s -> no match, resetting", key, ev.key, candidate);
		resetKeyBuffer();
	}
}, true);

// ---- kagi-style cursor highlight + yank popup, injected once ------------

(function injectStyle() {
	var style = document.createElement("style");
	style.textContent =
		".mu-cursor { outline: 2px solid #4f9dff !important; outline-offset: 2px !important; }" +
		".mu-yank-popup {" +
		"  position: fixed; bottom: 24px; right: 24px; z-index: 2147483647;" +
		"  background: #1e1e1e; color: #eee; font-family: monospace; font-size: 13px;" +
		"  padding: 8px 12px; border-radius: 4px; box-shadow: 0 2px 8px rgba(0,0,0,0.4);" +
		"  opacity: 1; transition: opacity 0.2s ease-out; pointer-events: none;" +
		"  max-width: 60vw; overflow-wrap: break-word;" +
		"}" +
		".mu-yank-popup .mu-yank-label { color: #4f9dff; font-weight: bold; }" +
		".mu-yank-popup.mu-yank-popup-hide { opacity: 0; }";
	(document.head || document.documentElement).appendChild(style);
})();

// Small toast confirming a yank, e.g.:
//   yanked
//   :https://example.com/page
// Built with textContent (not innerHTML) since the URL is arbitrary
// page content, not something to trust as markup.
var yankPopupTimer = null;
function showYankPopup(url) {
	var existing = document.querySelector(".mu-yank-popup");
	if (existing) existing.remove();
	if (yankPopupTimer) clearTimeout(yankPopupTimer);

	var el = document.createElement("div");
	el.className = "mu-yank-popup";

	var label = document.createElement("div");
	label.className = "mu-yank-label";
	label.textContent = "yanked";

	var urlLine = document.createElement("div");
	urlLine.textContent = ":" + url;

	el.appendChild(label);
	el.appendChild(urlLine);
	document.body.appendChild(el);

	yankPopupTimer = setTimeout(function () {
		el.classList.add("mu-yank-popup-hide");
		setTimeout(function () { el.remove(); }, 200);
	}, 1200);
}
