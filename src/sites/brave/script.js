define BRAVE {
    url("*://search.brave.com/*")

    loop("RESULT", ".title.search-snippet-title.line-clamp-1.svelte-14r20fy")

    // j/k cycle through search results, kagi-style
    focus(j, goto(next, "RESULT"))
    focus(k, goto(prev, "RESULT"))

    // jump straight to the search box
    focus(gi, "input#searchbox")

    scroll(gg, up, full)
    scroll(G, down, full)
}
