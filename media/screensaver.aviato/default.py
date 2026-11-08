"""Aviato private-safe native frame as a Kodi screensaver."""
import os
from pathlib import Path
import sys
import xbmc
import xbmcgui

sys.path.insert(0, str(Path(__file__).parent / 'resources' / 'lib'))
from saver import display_loop

class Saver(xbmcgui.WindowDialog):
    active = True
    def onAction(self, action):
        self.active = False
        self.close()

class Monitor(xbmc.Monitor):
    def onScreensaverDeactivated(self):
        window.active = False
        window.close()

window = Saver()
monitor = Monitor()
# A black backing prevents any previous GUI frame from showing through.
width, height = window.getWidth(), window.getHeight()
background = xbmcgui.ControlImage(0, 0, width, height, str(Path(__file__).parent / 'resources/black.png'))
window.addControl(background)
image = xbmcgui.ControlImage(0, 0, width, height, '')
window.addControl(image)
window.show()
try:
    display_loop(monitor, image,
                 Path('/run/user') / str(os.getuid()) / 'aviato-screen/screensaver.bmp',
                 lambda: window.active)
finally:
    window.close()
