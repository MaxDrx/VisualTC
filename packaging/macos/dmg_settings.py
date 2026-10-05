# dmgbuild settings for the VisualTC disk image (used by make_dmg.sh).
#
#   dmgbuild -s dmg_settings.py -D app=PATH/VisualTC.app -D background=bg.tiff \
#            -D icon=VisualTC.icns "VisualTC" VisualTC-macOS.dmg
#
# The window shows the app on the left, an arrow and the Applications folder
# on the right: the usual "drag to install" of macOS, explained in Portuguese
# on the background picture (tools/make_dmg_background.py).
import os.path

app = defines.get("app", "VisualTC.app")  # noqa: F821 (provided by dmgbuild)
appname = os.path.basename(app)

format = "UDZO"
filesystem = "HFS+"
files = [app]
# Named in Portuguese, like the folder in Finder on a Portuguese macOS.
symlinks = {"Aplicativos": "/Applications"}
hide_extensions = [appname]
icon = defines.get("icon")  # noqa: F821

background = defines.get("background")  # noqa: F821
window_rect = ((200, 140), (600, 400))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
sidebar_width = 0
icon_size = 128
text_size = 13
icon_locations = {
    appname: (150, 185),
    "Aplicativos": (450, 185),
}
