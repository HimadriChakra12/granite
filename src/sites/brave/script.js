define BRAVE {
    url("*://search.brave.com/*")

    close(x)

    loop("RESULT", ".title.search-snippet-title.line-clamp-1.svelte-14r20fy")
    focus(j, goto(next, "RESULT"))
    focus(k, goto(prev, "RESULT"))
    click(enter, selected("RESULT"))

    focus(gi, "input#searchbox")

    scroll(gg, up, full)
    scroll(G, down, full)
    opennew(q, selected("RESULT"))
}
