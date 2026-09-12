define UNIVERSAL {
    scroll(j, down, 50)
    scroll(k, up, 50)

    action(r, reload)
    navigate(H, prev)
    navigate(L, next)
    scroll(gg, up, full)
    scroll(G, down, full)
    action(yy, yankurl())
}
