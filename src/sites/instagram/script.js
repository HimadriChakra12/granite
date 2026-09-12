define INSTAGRAM {
    url("*://www.instagram.com/direct/*")
    url("*://instagram.com/direct/*")

    focus(gi, "div[role='textbox'][aria-placeholder='Message...']")
    focus(gI, "input[name='searchInput']")

    loop("CHAT", ".html-div.xdj266r.x14z9mp.xat24cr.x1lziwak.xexx8yu.xyri2b.x18d9i69.x1c1uobl.x6ikm8r.x10wlt62:not(:has(.x1kmbdvd))")
    focus(k, goto(prev, "CHAT"))
    focus(j, goto(next, "CHAT"))
    doubleclick(space, selected("CHAT"))

    action(x, close) 
}
