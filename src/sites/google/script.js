define GOOGLE {
    url("*://www.google.com/*")

    loop("RESULT", "h3.LC20lb.MBeuO.DKV0Md")
    focus(j, goto(next, "RESULT"))
    focus(k, goto(prev, "RESULT"))
    click(Enter, selected())

    focus(gi, "textarea")

    scroll(gg, up, full)
    scroll(G, down, full)
}
