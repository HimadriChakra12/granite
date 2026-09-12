define YOUTUBESEARCH{
    url("*://*.youtube.com/results?search_query=*")

    loop("SRCH", "ytd-video-renderer")
    focus(j, goto(next, "SRCH"))
    focus(k, goto(prev, "SRCH"))
    click(enter, selected("SRCH"))

}

define YOUTUBE{
    url("*://*.youtube.com/")

    loop("CONTENT", "[id='content']")
    focus(j, goto(next, "CONTENT"))
    focus(k, goto(prev, "CONTENT"))
    click(enter, selected("CONTENT"))
}

define YOUTUBEWATCH{
    url("*://*.youtube.com/watch?v=*")

    loop("SUGG", ".ytLockupViewModelContentImage")
    focus(j, goto(next, "SUGG"))
    focus(k, goto(prev, "SUGG"))
    click(enter, selected("CONTENT"))
}

