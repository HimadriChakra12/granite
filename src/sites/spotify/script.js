define SPOTIFY {
    url("*://open.spotify.com/*")

    loop("RESULT", "[data-testid='tracklist-row']")

    focus(j, goto(next, "RESULT"))
    focus(k, goto(prev, "RESULT"))

    variable("LIKE_PARENT", "div[class='XbPvmnihs9RIqUSkVHjv']")
    variable("LIKE_CHILD", "button[aria-label='Add to Liked Songs']")
    variable("LIKE", branch("LIKE_PARENT", "LIKE_CHILD"))

    click(l, get("LIKE"))
    click(L, "button[aria-label='Lyrics']")
    click(m, "button[aria-label='Mute'], button[aria-label='Unmute']")
    click(f, "button[aria-label='Enter Full screen']")

    doubleclick(enter, selected("RESULT"))
}
