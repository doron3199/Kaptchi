from kivy.uix.boxlayout import BoxLayout
from  kivy.properties import NumericProperty, ObjectProperty
from bus.bus import Bus


class Frontend(BoxLayout):
    zoom = NumericProperty(0)
    bus = ObjectProperty(None)

    def set(self, bus: Bus):
        self.bus = bus


    def on_change_camera_btn_click(self):
        self.bus.on_change_camera_btn_click()

    def on_zoom_slider_change(self):
        """notice! slider can change and invoke this function without zoom value change!"""
        self.zoom = self.ids.zoom.value_normalized

    def on_zoom(self, instance, value):
        """invoked only when zoom value change. send value to the bus"""
        self.bus.on_zoom_change(value)

    def update_main_image(self, texture):
        """receives an image from the bus and put in on the image widget"""
        self.ids.main_image.texture = texture



