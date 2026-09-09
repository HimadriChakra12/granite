define REDDIT {
    url("https://www.reddit.com/")

    loop("ARTICLE", "article")
    focus(k, goto(prev, "ARTICLE"))
    focus(j, goto(next, "ARTICLE"))

    action(x, close)
}
