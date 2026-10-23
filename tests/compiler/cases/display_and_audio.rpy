image bg room = "bg room.png"
image eileen happy = Image("eileen happy.png", xalign=0.5, yalign=1.0)
image eileen sad = im.FactorScale("eileen happy.png", 0.5)
image side eileen happy = im.Composite((100, 100), (0, 0), "eileen happy.png")
image flash = Solid("#fff")
define flashbulb = Fade(0.2, 0.0, 0.4, color="#fff")
define slow = Dissolve(2.0)
define spot = Position(xpos=0.25, xanchor="center", ypos=1.0)

transform slide_in(x=0.8):
    xalign 1.5
    linear 0.5 xalign x
    pause 0.25
    repeat 2

transform pulse:
    on show:
        alpha 0.0
        ease 0.3 alpha 1.0
    parallel:
        zoom 1.0
        linear 1.0 zoom 1.1
    parallel:
        xoffset 0
        easeout 1.0 xoffset 10

init python:
    renpy.music.register_channel("ambient", "sfx", True)
    define.move_transitions("glide", 1.0)

label start:
    scene bg room with fade
    show eileen happy at left, slide_in(0.6)
    with slow
    show eileen sad as friend at spot behind eileen zorder 2
    show eileen happy:
        xalign 0.2
        linear 1.0 xalign 0.8
    show text "Chapter 1" at truecenter
    hide text with dissolve
    show eileen happy at pulse with glideinleft
    with flashbulb
    with vpunch
    play music "theme.ogg" fadein 1.0 loop
    queue music ["theme.ogg", "theme.ogg"]
    play sound "click.wav" volume 0.5
    play ambient "click.wav"
    stop music fadeout 2.0
    voice "line1.ogg"
    pause 1.5
    pause
    window hide dissolve
    window auto
    $ renpy.pause(0.5)
    scene
    return
