define e = Character(_("Eileen"))

label start:
    e "Good morning."
    menu:
        e "Coffee or tea?"
        "Coffee":
            "You chose coffee."
        "Tea":
            "You chose tea."
    return

translate vietnamese start_461b1ae4:
    e "Chào buổi sáng."

translate vietnamese strings:
    old "Eileen"
    new "Ai Linh"
    old "Coffee"
    new "Cà phê"
    old "Tea"
    new "Trà"

translate vietnamese start_fbdaedd6:
    e "Cà phê hay trà?" nointeract
