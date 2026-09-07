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

// ---- loop / goto cursor --------------------------------------------------

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

// dir is clamped, not wrapped, by default -- change Math.min/Math.max
// below to wrap instead if you'd rather j on the last result cycle back
// to the first.
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

var ACTIONS = { focus: doFocus, click: doClick, longpress: doLongpress, doubleclick: doDoubleclick };

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
	// Escape always exits "insert mode" -- checked before the editable-
	// target bailout below, since that's precisely when it's needed.
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

// ---- kagi-style cursor highlight, injected once -------------------------

(function injectStyle() {
	var style = document.createElement("style");
	style.textContent =
		".mu-cursor { outline: 2px solid #4f9dff !important; outline-offset: 2px !important; }";
	(document.head || document.documentElement).appendChild(style);
})();
