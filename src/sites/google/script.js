define GOOGLE {
    url("*://www.google.com/*")

    loop("RESULT", "h3.LC20lb.MBeuO.DKV0Md")

    // j/k cycle through search results, kagi-style
    focus(j, goto(next, "RESULT"))
    focus(k, goto(prev, "RESULT"))

    click(Enter, selected())

    // jump straight to the search box
    focus(gi, "input#searchbox")

    scroll(gg, up, full)
    scroll(G, down, full)
}
