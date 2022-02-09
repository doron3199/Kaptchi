from kivy.uix.boxlayout import BoxLayout
from kivy.properties import NumericProperty, ObjectProperty
from bus.bus import Bus


class Frontend(BoxLayout):
    bus = ObjectProperty(None)

    def set(self, bus: Bus):
        self.bus = bus

    def on_change_camera_btn_click(self):
        self.bus.on_change_camera_btn_click()

    def on_whiteboard_filter_btn_click(self):
        self.bus.on_whiteboard_filter_btn_click()

    def on_zoom_slider_change(self, widget):
        self.bus.on_zoom_change(widget.value_normalized)

    def update_main_image(self, texture):
        """receives an image from the bus and put in on the image widget"""
        self.ids.main_image.texture = texture



