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
function effectiveBindings() {
	var seen = {};
	var result = [];
	function addAll(bindings) {
		bindings.forEach(function (b) {
			if (!seen[b.keys]) { seen[b.keys] = true; result.push(b); }
		});
	}

	var specific = activeSite();
	if (specific) addAll(specific.bindings);
	universalSites().forEach(function (site) { addAll(site.bindings); });
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
	if (b.kind === "history") {
		if (b.dir === "prev") history.back();
		else history.forward();
		return;
	}
	if (b.kind === "close") {
		// Privileged via the "window.close" grant declared in build.c
		// (Tampermonkey/Violentmonkey back this with their own
		// extension internals) -- unlike ordinary page-JS
		// window.close(), this actually closes the tab regardless of
		// how it was opened or its navigation history.
		window.close();
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
	// Escape always exits "insert mode" AND breaks any active loop/goto
	// cursor -- checked before the editable-target bailout below, since
	// that's precisely when it's needed.
	if (ev.key === "Escape") {
		if (isEditableTarget(ev.target) && ev.target.blur) ev.target.blur();
		clearAllHighlights();
		resetKeyBuffer();
		return;
	}

	if (ev.altKey || ev.ctrlKey || ev.metaKey) return;
	if (isEditableTarget(ev.target)) return; // don't hijack typing; gi/gI got you here
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

// ---- kagi-style cursor highlight, injected once -------------------------

(function injectStyle() {
	var style = document.createElement("style");
	style.textContent =
		".mu-cursor { outline: 2px solid #4f9dff !important; outline-offset: 2px !important; }";
	(document.head || document.documentElement).appendChild(style);
})();
