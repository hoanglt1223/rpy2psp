default score = 0
default seen = set()

label start:
    $ score += 2
    if score > 1 and not score == 5:
        "Big."
    elif score == 1:
        "One."
    else:
        "Small."
    while score < 5:
        $ score = score + 1
    call greet("Ann", times=2) from after_greet
    call expression "greet" 
    menu chooser:
        set seen
        "Pick one."
        "Red" if score >= 5:
            $ color = "red"
            jump .done
        "Blue":
            $ color = "blue"
    label .done:
        "Done: [color]"
    jump finish

label greet(name="you", times=1):
    "Hi [name] x[times]."
    return times * 2

label finish:
    $ total = len(seen) + _return
    return
