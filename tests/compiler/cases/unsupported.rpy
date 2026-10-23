init python:
    def helper():
        return 1

screen custom_hud():
    text "HUD"

label start:
    show screen custom_hud
    $ helper()
    $ a, b = 1, 2
    with ImageDissolve("mask.png", 1.0)
    with irisin
    layeredimage eileen:
        always "e.png"
    "Text with {image=icon.png} tag."
    show eileen at Transform(function=None)
    "[helper()]"
    jump nowhere
