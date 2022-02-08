from kivy.uix.boxlayout import BoxLayout

from bus.bus import Bus


class Frontend(BoxLayout):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.bus = None

    def set(self, bus: Bus):
        self.bus = bus


    def on_change_camera_btn_click(self):
        self.bus.on_change_camera_btn_click()

    def update_main_image(self, texture):
        self.ids.main_image.texture = texture



