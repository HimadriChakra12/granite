define GOOGLECALENDER {
    url("*://calendar.google.com/calendar/*")

    loop("DAY", "[class='MGaLHf ChfiMc']")

    focus(j, goto(next, "DAY"))
    focus(k, goto(prev, "DAY"))

    click(h, "[aria-label*='Previous']")
    click(l, "[aria-label*='Next']")

    click(Space, selected("DAY"))
}
