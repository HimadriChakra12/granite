define YOUTUBESEARCH{
    url("*://*.youtube.com/results?search_query=*")

    loop("SRCH", "ytd-video-renderer")
    focus(j, goto(next, "SRCH"))
    focus(k, goto(prev, "SRCH"))

    click(enter, selected("SRCH"))
}
