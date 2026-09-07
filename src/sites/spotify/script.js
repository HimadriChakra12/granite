define SPOTIFY {
    url("*://open.spotify.com/*")
    
    loop("RESULT", "[data-testid='tracklist-row']")

    focus(j , goto(next, "RESULT")) 
    focus(k , goto(prev, "RESULT")) 
    
    click(l , "button[aria-label='Add to playlist']")
    click(L , "button[aria-label='Lyrics']")
    click(m , "button[aria-label='Mute'] , button[aria-label='Unmute']")
    click(f , "button[aria-label='Enter Full screen']")
}
