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

function activeSite() {
	for (var i = 0; i < Sites.list.length; i++) {
		if (siteMatches(Sites.list[i])) return Sites.list[i];
	}
	return null;
}

function effectiveBindings() {
	var site = activeSite();
	var siteBindings = site ? site.bindings : [];
	var seen = {};
	siteBindings.forEach(function (b) { seen[b.keys] = true; });
	var defaults = DEFAULT_BINDINGS.filter(function (b) { return !seen[b.keys]; });
	return siteBindings.concat(defaults);
}


var loopCursor = {};       // loopName -> current index
var lastHighlighted = null;

function highlight(el) {
	if (lastHighlighted && lastHighlighted !== el) {
		lastHighlighted.classList.remove("mu-cursor");
	}
	if (el) {
		el.classList.add("mu-cursor");
		el.scrollIntoView({ block: "center", behavior: "smooth" });
	}
	lastHighlighted = el;
}

function gotoLoop(loopName, dir) {
	var site = activeSite();
	var selector = site && site.loops && site.loops[loopName];
	if (!selector) return null;

	var els = Array.prototype.slice.call(document.querySelectorAll(selector));
	if (!els.length) return null;

	var idx = loopCursor.hasOwnProperty(loopName) ? loopCursor[loopName] : -1;
	if (dir === "next") idx = Math.min(idx + 1, els.length - 1);
	else idx = Math.max(idx - 1, 0);
	loopCursor[loopName] = idx;

	var el = els[idx];
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
	if (amount === Infinity) {
		window.scrollTo({ top: dir === "down" ? 1e9 : 0, behavior: "smooth" });
		return;
	}
	var px = window.innerHeight * (amount / 100);
	window.scrollBy({ top: dir === "down" ? px : -px, behavior: "smooth" });
}

var ACTIONS = { focus: doFocus, click: doClick, longpress: doLongpress, doubleclick: doDoubleclick };

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

function isEditableTarget(el) {
	if (!el) return false;
	var tag = el.tagName;
	return el.isContentEditable || tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
}

function resetKeyBuffer() {
	keyBuffer = "";
	if (keyTimer) { clearTimeout(keyTimer); keyTimer = null; }
}

document.addEventListener("keydown", function (ev) {
	if (ev.key === "Escape") {
		if (isEditableTarget(ev.target) && ev.target.blur) ev.target.blur();
		resetKeyBuffer();
		return;
	}

	if (ev.altKey || ev.ctrlKey || ev.metaKey) return;
	if (isEditableTarget(ev.target)) return; // don't hijack typing; gi/gI got you here
	if (ev.key.length !== 1) return; // ignore bare modifiers/arrows/etc

	var candidate = keyBuffer + ev.key;
	var bindings = effectiveBindings();

	var exact = bindings.filter(function (b) { return b.keys === candidate; });
	var stillPossible = bindings.some(function (b) { return b.keys.indexOf(candidate) === 0; });

	if (exact.length) {
		ev.preventDefault();
		performBinding(exact[0]);
		resetKeyBuffer();
		return;
	}

	if (stillPossible) {
		ev.preventDefault();
		keyBuffer = candidate;
		if (keyTimer) clearTimeout(keyTimer);
		keyTimer = setTimeout(resetKeyBuffer, SEQUENCE_TIMEOUT_MS);
	} else {
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
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "RESULT" },
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "RESULT" },
    { keys: "gi", action: "focus", kind: "selector", value: "input#searchbox" },
    { keys: "gg", action: "scroll", kind: "scroll", dir: "up", amount: Infinity },
    { keys: "G", action: "scroll", kind: "scroll", dir: "down", amount: Infinity }
  ]
});

Sites.register({
  name: "DISCORD",
  match: ["*://discord.com/*"],
  loops: {},
  bindings: [
    { keys: "ss", action: "click", kind: "selector", value: "div[data-dnd-name='সিসিমপুর']" },
    { keys: "sd", action: "click", kind: "selector", value: "div[data-dnd-name='Da hood']" }
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
    { keys: "f", action: "click", kind: "selector", value: "button[aria-label='Enter Full screen']" }
  ]
});

Sites.register({
  name: "INSTAGRAM",
  match: ["*://www.instagram.com/*", "*://instagram.com/*"],
  loops: {"MSGBARBUTTON": "div[class='html-div xdj266r x14z9mp xat24cr x1lziwak xexx8yu xyri2b x18d9i69 x1c1uobl x9f619 xjbqb8w x78zum5 x15mokao x1ga7v0g x16uus16 xbiv7yw x1plvlek xryxfnj x1c4vz4f x2lah0s xdt5ytf xqjyukv x1qjc9v5 x1oa3qoh x1nhvcw1 x3h4tne x145d82y xixxii4']", "MSG": "div[class='x1i10hfl x1qjc9v5 xjbqb8w xjqpnuy xc5r6h4 xqeqjp1 x1phubyo x13fuv20 x18b5jzi x1q0q8m5 x1t7ytsu x972fbf x10w94by x1qhh985 x14e42zd x9f619 x1ypdohk xdl72j9 x2lah0s x3ct3a4 x2lwn1j xeuugli xexx8yu xyri2b x18d9i69 x1c1uobl x1n2onr6 x16tdsg8 x1hl2dhg xggy1nq x1ja2u2z x1t137rt x1q0g3np x87ps6o x1lku1pv x1a2a7pz x4gyw5p xd3so5o x1l895ks x6nl9eh x1a5l9x9 x7vuprf x1mg3h75 x1lliihq xdj266r x14z9mp xat24cr x1lziwak xg6hnt2 x18wri0h']"},
  bindings: [
    { keys: "gi", action: "focus", kind: "selector", value: "div[role='textbox'][aria-placeholder='Message...']" },
    { keys: "gI", action: "focus", kind: "selector", value: "input[name='searchInput']" },
    { keys: "m", action: "click", kind: "selector", value: "MSGBUTTON" },
    { keys: "k", action: "focus", kind: "goto", dir: "prev", loop: "MSG" },
    { keys: "j", action: "focus", kind: "goto", dir: "next", loop: "MSG" }
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

// ---- end.js ----
})();

