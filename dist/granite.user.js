// ==UserScript==
// @name         granite
// @namespace    granite
// @version      2.0.0
// @description  Persite Keybinding Program
// @match        *://*/*
// @//NAME       //Description
// @run-at       document-start
// ==/UserScript==

// ---- start.js ----
(function () {
  'use strict';

// ---- core.js ----

var Sites = {
	list: [],
	register: function (site) { Sites.list.push(site); }
};

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


function globToRegExp(glob) {
	var escaped = glob.replace(/[.+^${}()|[\]\\]/g, "\\$&").replace(/\*/g, ".*");
	return new RegExp("^" + escaped + "$");
}

function siteMatches(site) {
	return (site.match || []).some(function (pattern) {
		return globToRegExp(pattern).test(location.href);
	});
}

function isUniversal(site) {
	return !(site.match && site.match.length);
}

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

function findLoopSelector(loopName) {
	var specific = activeSite();
	if (specific && specific.loops && specific.loops[loopName]) return specific.loops[loopName];
	var universal = universalSites();
	for (var i = 0; i < universal.length; i++) {
		if (universal[i].loops && universal[i].loops[loopName]) return universal[i].loops[loopName];
	}
	return null;
}


var loopCursor = {};       // loopName -> the actual highlighted Element (not an index -- see gotoLoop)
var lastHighlighted = null;

function resolveSelected(loopNames) {
	var names = (loopNames && loopNames.length) ? loopNames : Object.keys(loopCursor);
	return names.map(function (n) { return loopCursor[n]; })
		.filter(function (el) { return el && el.isConnected; });
}

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
		var delta = (elRect.top - containerRect.top) - (containerRect.height / 2 - elRect.height / 2);
		animateScrollTop(container, container.scrollTop + delta, 180);
	}
	lastHighlighted = el;
}

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

var MU = { gotoLoop: gotoLoop, highlight: highlight, isVisible: isVisible };

function performBinding(b) {
	if (b.kind === "function") {
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
		window.close();
		return;
	}
	if (b.kind === "selected") {
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


var keyBuffer = "";
var keyTimer = null;
var SEQUENCE_TIMEOUT_MS = 1000;

var lastFiredKeys = null;
var lastFiredTime = 0;
var MIN_REPEAT_MS = 120;

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

function clearAllHighlights() {
	if (lastHighlighted) {
		lastHighlighted.classList.remove("mu-cursor");
		lastHighlighted = null;
	}
	loopCursor = {};
}

document.addEventListener("keydown", function (ev) {
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


(function injectStyle() {
	var style = document.createElement("style");
	style.textContent =
		".mu-cursor { outline: 2px solid #4f9dff !important; outline-offset: 2px !important; }";
	(document.head || document.documentElement).appendChild(style);
})();

// ---- generated: compiled site definitions ----
Sites.register({
  name: "BRAVE",
  match: ["*://search.brave.com/*"],
  loops: {"RESULT": ".title.search-snippet-title.line-clamp-1.svelte-14r20fy"},
  bindings: [
    { keys: "x", action: "close", kind: "close" },
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "RESULT" },
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "RESULT" },
    { keys: "enter", action: "click", kind: "selected", loops: ["RESULT"] },
    { keys: "gi", action: "focus", kind: "selector", value: "input#searchbox" },
    { keys: "gg", action: "scroll", kind: "scroll", dir: "up", amount: Infinity },
    { keys: "G", action: "scroll", kind: "scroll", dir: "down", amount: Infinity },
    { keys: "q", action: "opennew", kind: "selected", loops: ["RESULT"] }
  ]
});

Sites.register({
  name: "GOOGLE",
  match: ["*://www.google.com/*"],
  loops: {"RESULT": "h3.LC20lb.MBeuO.DKV0Md"},
  bindings: [
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "RESULT" },
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "RESULT" },
    { keys: "Enter", action: "click", kind: "selected", loops: [] },
    { keys: "gi", action: "focus", kind: "selector", value: "input#searchbox" },
    { keys: "gg", action: "scroll", kind: "scroll", dir: "up", amount: Infinity },
    { keys: "G", action: "scroll", kind: "scroll", dir: "down", amount: Infinity }
  ]
});

Sites.register({
  name: "GOOGLECALENDER",
  match: ["*://calendar.google.com/calendar/*"],
  loops: {"DAY": "[class='MGaLHf ChfiMc']"},
  bindings: [
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "DAY" },
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "DAY" },
    { keys: "h", action: "click", kind: "selector", value: "[aria-label*='Previous']" },
    { keys: "l", action: "click", kind: "selector", value: "[aria-label*='Next']" },
    { keys: "Space", action: "click", kind: "selected", loops: ["DAY"] }
  ]
});

Sites.register({
  name: "SPOTIFY",
  match: ["*://open.spotify.com/*"],
  loops: {"RESULT": "[data-testid='tracklist-row']"},
  bindings: [
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "RESULT" },
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "RESULT" },
    { keys: "l", action: "click", kind: "selector", value: "button[aria-label='Add to playlist']" },
    { keys: "L", action: "click", kind: "selector", value: "button[aria-label='Lyrics']" },
    { keys: "m", action: "click", kind: "selector", value: "button[aria-label='Mute'] , button[aria-label='Unmute']" },
    { keys: "f", action: "click", kind: "selector", value: "button[aria-label='Enter Full screen']" },
    { keys: "enter", action: "click", kind: "selected", loops: [] }
  ]
});

Sites.register({
  name: "YOUTUBESEARCH",
  match: ["*://*.youtube.com/results?search_query=*"],
  loops: {"SRCH": "ytd-video-renderer"},
  bindings: [
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "SRCH" },
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "SRCH" },
    { keys: "enter", action: "click", kind: "selected", loops: ["SRCH"] }
  ]
});

Sites.register({
  name: "INSTAGRAM",
  match: ["*://www.instagram.com/direct/*", "*://instagram.com/direct/*"],
  loops: {},
  bindings: [
    { keys: "gi", action: "focus", kind: "selector", value: "div[role='textbox'][aria-placeholder='Message...']" },
    { keys: "gI", action: "focus", kind: "selector", value: "input[name='searchInput']" }
  ]
});

Sites.register({
  name: "REDDIT",
  match: ["https://www.reddit.com/"],
  loops: {"ARTICLE": "article"},
  bindings: [
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "ARTICLE" },
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "ARTICLE" }
  ]
});

Sites.register({
  name: "UNIVERSAL",
  match: [],
  loops: {},
  bindings: [
    { keys: "x", action: "close", kind: "close" },
    { keys: "j", action: "scroll", kind: "scroll", dir: "down", amount: 50 },
    { keys: "k", action: "scroll", kind: "scroll", dir: "up", amount: 50 }
  ]
});

// ---- end.js ----
})();

