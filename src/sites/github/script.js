define GITHUB{
    url("*://github.com/search?q=*")

    loop("RESULT", "[class='Result-module__Result__I0WVD']")
    focus(j, goto(next, "RESULT"))
    focus(k, goto(prev, "RESULT"))
    opennew(space, selected("RESULT"))

    variable("SEARCH", "[aria-label='Open quick search dialog, type / to search']")
    click(gi, "SEARCH")
}
