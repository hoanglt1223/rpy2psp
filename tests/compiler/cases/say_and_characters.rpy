define e = Character("Eileen", color="#c8ffc8", image="eileen", what_prefix="\"", what_suffix="\"")
define n = Character(None, kind=nvl)
define p = DynamicCharacter("player_name")
default player_name = "Pat"

label start:
    "Narration with {b}bold{/b} and an escaped \{brace\} and \[bracket\]."
    e "Hello, [player_name]!"
    e happy "Showing a new expression."
    e @ sad "Temporary attributes."
    "Narrator" "A who-string speaker."
    centered "Centered text."
    e "First part..."
    extend " and the rest."
    n "NVL line."
    nvl clear
    p "Dynamic name."
    e "With a transition." with dissolve
    e "Not interacting." nointeract
    """Monologue one.

    Monologue two."""
    return
