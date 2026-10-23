define base = 10
default inventory = ["key"]
default stats = {"str": 1}
default persistent.endings = 0

label start:
    $ name = renpy.input("Name?", default="Pat", length=10).strip()
    $ inventory.append("map")
    $ stats["str"] += base // 3
    $ ratio = (stats["str"] * 2.5 - 1) % 7
    $ lucky = renpy.random.randint(1, 6) in (5, 6)
    $ label_ok = renpy.has_label("start") and len(inventory) >= 2 or False
    $ persistent.endings += 1
    $ first = inventory[0] if inventory else None
    $ part = name[1:3]
    $ renpy.notify("Saved " + str(persistent.endings))
    $ mobile = renpy.variant("small")
    "Stats: [stats[str]] [name!u]"
    return
